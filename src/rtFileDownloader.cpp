/*

 pxCore Copyright 2005-2018 John Robinson

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

// rtFileDownloader.cpp

// TODO what is this for??
#define XRELOG_NOCTRACE
#include <curl/curl.h>
#include "rtFileDownloader.h"
#include "rtThreadTask.h"
#include "rtThreadPool.h"
#include "pxTimer.h"
#include "rtLog.h"
#include <sstream>
#include <iostream>
#include <thread>
#include "rtUrlUtils.h"
#ifndef WIN32
#include <signal.h>
#endif //!WIN32
using namespace std;

#define CA_CERTIFICATE "cacert.pem"
#define MAX_URL_SIZE 8000
const int kCurlTimeoutInSeconds = 30;
const double kDefaultDownloadHandleExpiresTime = 5 * 60;
const int kDownloadHandleTimerIntervalInMilliSeconds = 30 * 1000;

std::thread* downloadHandleExpiresCheckThread = NULL;
bool continueDownloadHandleCheck = true;
rtMutex downloadHandleMutex;

#define HTTP_DOWNLOAD_CANCELED 499

struct MemoryStruct
{
    MemoryStruct()
        : headerSize(0)
        , headerBuffer(NULL)
        , contentsSize(0)
        , contentsBuffer(NULL)
        , downloadRequest(NULL)
        , readSize(0)
    {
        headerBuffer = (char*)malloc(1);
        contentsBuffer = (char*)malloc(1);
    }

    ~MemoryStruct()
    {
      if (headerBuffer != NULL)
      {
        free(headerBuffer);
        headerBuffer = NULL;
      }
      if (contentsBuffer != NULL)
      {
        free(contentsBuffer);
        contentsBuffer = NULL;
      }
    }

  size_t headerSize;
  char* headerBuffer;
  size_t contentsSize;
  char* contentsBuffer;
  rtFileDownloadRequest *downloadRequest;
  size_t readSize;
};

static size_t HeaderCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t downloadSize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  mem->headerBuffer = (char*)realloc(mem->headerBuffer, mem->headerSize + downloadSize + 1);
  if(mem->headerBuffer == NULL) {
    /* out of memory! */
    cout << "out of memory when downloading image\n";
    return 0;
  }

  memcpy(&(mem->headerBuffer[mem->headerSize]), contents, downloadSize);
  mem->headerSize += downloadSize;
  mem->headerBuffer[mem->headerSize] = 0;

  return downloadSize;
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t downloadSize = size * nmemb;
  size_t downloadCallbackSize = 0;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  downloadCallbackSize = mem->downloadRequest->executeExternalWriteCallback(contents, size, nmemb );

  mem->contentsBuffer = (char*)realloc(mem->contentsBuffer, mem->contentsSize + downloadSize + 1);
  if(mem->contentsBuffer == NULL) {
    /* out of memory! */
    cout << "out of memory when downloading image\n";
    return 0;
  }

  memcpy(&(mem->contentsBuffer[mem->contentsSize]), contents, downloadSize);
  mem->contentsSize += downloadSize;
  mem->contentsBuffer[mem->contentsSize] = 0;

  if (mem->downloadRequest->useCallbackDataSize() == true)
  {
     return downloadCallbackSize;
  }
  else
  {
     return downloadSize;
  }
}

static size_t ReadMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t bufferSize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  size_t sizeLeft = mem->downloadRequest->readDataSize() - mem->readSize;
  if (sizeLeft > 0) {
    size_t copyThisMuch = sizeLeft;
    if (copyThisMuch > bufferSize)
      copyThisMuch = bufferSize;
    memcpy(contents, mem->downloadRequest->readData() + mem->readSize, copyThisMuch);
    mem->readSize += copyThisMuch;
    return copyThisMuch;
  }

  return 0;
}


void startFileDownloadInBackground(void* data)
{
    rtFileDownloadRequest* downloadRequest = (rtFileDownloadRequest*)data;

    if(downloadRequest->isByteRangeEnabled() == false)
       rtFileDownloader::instance()->downloadFile(downloadRequest);
    else
       rtFileDownloader::instance()->downloadFileAsByteRange(downloadRequest);
}

rtFileDownloader* rtFileDownloader::mInstance = NULL;
std::vector<rtFileDownloadRequest*>* rtFileDownloader::mDownloadRequestVector = new std::vector<rtFileDownloadRequest*>();
rtMutex* rtFileDownloader::mDownloadRequestVectorMutex = new rtMutex();


void onDownloadHandleCheck()
{
  rtLogDebug("inside onDownloadHandleCheck");
  bool checkHandles = true;
  while (checkHandles)
  {
	pxSleepMS(kDownloadHandleTimerIntervalInMilliSeconds);
    rtFileDownloader::instance()->checkForExpiredHandles();
    downloadHandleMutex.lock();
    checkHandles = continueDownloadHandleCheck;
    downloadHandleMutex.unlock();
  }
}

rtFileDownloadRequest::rtFileDownloadRequest(const char* imageUrl, void* callbackData, void (*callbackFunction)(rtFileDownloadRequest*))
      : mTag(), mFileUrl(imageUrl), mProxyServer(),
    mErrorString(), mHttpStatusCode(0), mCallbackFunction(callbackFunction), mExternalWriteCallback(NULL), mExternalWriteCallbackUserPtr(NULL),
    mProgressCallback(NULL), mProgressCallbackUserPtr(NULL), mDownloadedData(0), mDownloadedDataSize(), mDownloadStatusCode(0) ,mCallbackData(callbackData),
    mCallbackFunctionMutex(), mHeaderData(0), mHeaderDataSize(0), mHeaderOnly(false), mDownloadHandleExpiresTime(-2)
#ifdef ENABLE_HTTP_CACHE
    , mCacheEnabled(true), mDeferCacheRead(false), mCachedFileReadSize(0)
#endif
    , mIsDataInCache(false)
    , mIsProgressMeterSwitchOff(false), mHTTPFailOnError(false), mDefaultTimeout(false), mConnectionTimeout(0)
    , mCORS(), mCanceled(false), mUseCallbackDataSize(false), mCanceledMutex()
    , mMethod()
    , mReadData(NULL)
    , mReadDataSize(0)
    , mDownloadMetricsEnabled(true)
    , mDownloadMetrics(new rtMapObject())
    , mDownloadOnly(false)
    , mActualFileSize(0)
    , mIsByteRangeEnabled(false)
    , mByteRangeIntervals(0)
    , curlErrRetryCount(0)
    , mCurlRetry(false)
    , mUseEncoding(true)
    , mUserAgent()
    , mRedirectFollowLocation(true)
    , mKeepTCPAlive(true)
    , mCROSRequired(true)
{
  mAdditionalHttpHeaders.clear();
#ifdef ENABLE_HTTP_CACHE
  memset(mHttpErrorBuffer, 0, sizeof(mHttpErrorBuffer));
#endif
}

rtFileDownloadRequest::~rtFileDownloadRequest()
{
  if (mDownloadedData  != NULL)
  {
    free(mDownloadedData);
  }
  mDownloadedData = NULL;
  if (mHeaderData != NULL)
  {
    free(mHeaderData);
  }
  mHeaderData = NULL;
  mAdditionalHttpHeaders.clear();
  mHeaderOnly = false;
  mDownloadMetrics = NULL;
}

void rtFileDownloadRequest::setTag(const char* tag) { mTag = tag; }
rtString rtFileDownloadRequest::tag() const { return mTag; }

void rtFileDownloadRequest::setFileUrl(const char* imageUrl) { mFileUrl = imageUrl; }
rtString rtFileDownloadRequest::fileUrl() const { return mFileUrl; }

void rtFileDownloadRequest::setProxy(const char* proxyServer)
{
  mProxyServer = proxyServer;
}

rtString rtFileDownloadRequest::proxy() const
{
  return mProxyServer;
}

void rtFileDownloadRequest::setErrorString(const char* errorString)
{
  mErrorString = errorString;
}

rtString rtFileDownloadRequest::errorString()
{
  return mErrorString;
}

void rtFileDownloadRequest::setCallbackFunction(void (*callbackFunction)(rtFileDownloadRequest*))
{
  mCallbackFunction = callbackFunction;
}

void rtFileDownloadRequest::setExternalWriteCallback(size_t (*callbackFunction)(void *ptr, size_t size, size_t nmemb, void *userData), void *userPtr)
{
  mExternalWriteCallback = callbackFunction;
  mExternalWriteCallbackUserPtr = userPtr;
}

void rtFileDownloadRequest::setProgressCallback(int (*callbackFunction)(void* ptr, double dltotal, double dlnow, double ultotal, double ulnow), void *userPtr)
{
  mProgressCallback = callbackFunction;
  mProgressCallbackUserPtr = userPtr;
}

void* rtFileDownloadRequest::progressCallback(void)
{
  return mProgressCallback;
}

void* rtFileDownloadRequest::progressCallbackUserPtr(void)
{
  return mProgressCallbackUserPtr;
}

void rtFileDownloadRequest::setCallbackFunctionThreadSafe(void (*callbackFunction)(rtFileDownloadRequest*))
{
  mCallbackFunctionMutex.lock();
  mCallbackFunction = callbackFunction;
  mCallbackFunctionMutex.unlock();
}

long rtFileDownloadRequest::httpStatusCode()
{
  return mHttpStatusCode;
}

void rtFileDownloadRequest::setHttpStatusCode(long statusCode)
{
  mHttpStatusCode = statusCode;
}

bool rtFileDownloadRequest::executeCallback(int statusCode)
{
  mDownloadStatusCode = statusCode;
  mCallbackFunctionMutex.lock();
  if (mCallbackFunction != NULL)
  {
    (*mCallbackFunction)(this);
    mCallbackFunctionMutex.unlock();
    return true;
  }
  mCallbackFunctionMutex.unlock();
  return false;
}

size_t rtFileDownloadRequest::executeExternalWriteCallback(void * ptr, size_t size, size_t nmemb)
{
  if(mExternalWriteCallback)
  {
    return mExternalWriteCallback(ptr, size, nmemb, mExternalWriteCallbackUserPtr);
  }
  return 0;
}

void rtFileDownloadRequest::setDownloadedData(char* data, size_t size)
{
  mDownloadedData = data;
  mDownloadedDataSize = size;
}

void rtFileDownloadRequest::downloadedData(char*& data, size_t& size)
{
  data = mDownloadedData;
  size = mDownloadedDataSize;
}

char* rtFileDownloadRequest::downloadedData()
{
  return mDownloadedData;
}

size_t rtFileDownloadRequest::downloadedDataSize()
{
  return mDownloadedDataSize;
}

void rtFileDownloadRequest::setHeaderData(char* data, size_t size)
{
  mHeaderData = data;
  mHeaderDataSize = size;
}

char* rtFileDownloadRequest::headerData()
{
  return mHeaderData;
}

size_t rtFileDownloadRequest::headerDataSize()
{
  return mHeaderDataSize;
}

/*  Function to set additional http headers */
void rtFileDownloadRequest::setAdditionalHttpHeaders(std::vector<rtString>& additionalHeaders)
{
  mAdditionalHttpHeaders = additionalHeaders;
}

std::vector<rtString>& rtFileDownloadRequest::additionalHttpHeaders()
{
  return mAdditionalHttpHeaders;
}

void rtFileDownloadRequest::setDownloadStatusCode(int statusCode)
{
  mDownloadStatusCode = statusCode;
}

int rtFileDownloadRequest::downloadStatusCode()
{
  return mDownloadStatusCode;
}

void* rtFileDownloadRequest::callbackData()
{
  return mCallbackData;
}

void rtFileDownloadRequest::setCallbackData(void* callbackData)
{
  mCallbackData = callbackData;
}

/* Function used to set to download only header or not */
void rtFileDownloadRequest::setHeaderOnly(bool val)
{
  mHeaderOnly = val;
}

bool rtFileDownloadRequest::headerOnly()
{
  return mHeaderOnly;
}

void rtFileDownloadRequest::setDownloadHandleExpiresTime(double timeInSeconds)
{
  mDownloadHandleExpiresTime = timeInSeconds;
}

double rtFileDownloadRequest::downloadHandleExpiresTime()
{
  return mDownloadHandleExpiresTime;
}

#ifdef ENABLE_HTTP_CACHE
/* Function used to enable or disable using file cache */
void rtFileDownloadRequest::setCacheEnabled(bool val)
{
  mCacheEnabled = val;
}

bool rtFileDownloadRequest::cacheEnabled()
{
  return mCacheEnabled;
}

void rtFileDownloadRequest::setDataIsCached(bool val)
{
  mIsDataInCache = val;
}

size_t rtFileDownloadRequest::getCachedFileReadSize(void )
{
  return mCachedFileReadSize;
}

void rtFileDownloadRequest::setCachedFileReadSize(size_t cachedFileReadSize)
{
  mCachedFileReadSize = cachedFileReadSize;
}

void rtFileDownloadRequest::setDeferCacheRead(bool val)
{
  mDeferCacheRead = val;
}

bool rtFileDownloadRequest::deferCacheRead()
{
  return mDeferCacheRead;
}

FILE* rtFileDownloadRequest::cacheFilePointer(void)
{
  rtHttpCacheData cachedData(this->fileUrl().cString());

  if (true == this->cacheEnabled())
  {
    if ((NULL != rtFileCache::instance()) && (RT_OK == rtFileCache::instance()->httpCacheData(this->fileUrl(), cachedData)))
    {
      rtLogInfo("fileUrl[%s] fileName[%s] \n", this->fileUrl().cString(), cachedData.fileName().cString());
      return cachedData.filePointer();
    }
  }
  return NULL;
}
#endif //ENABLE_HTTP_CACHE

bool rtFileDownloadRequest::isDataCached()
{
  return mIsDataInCache;
}

void rtFileDownloadRequest::setProgressMeter(bool val)
{
  mIsProgressMeterSwitchOff = val;
}

void rtFileDownloadRequest::setUseCallbackDataSize(bool val)
{
  mUseCallbackDataSize = val;
}

bool rtFileDownloadRequest::useCallbackDataSize()
{
  return mUseCallbackDataSize;
}

bool rtFileDownloadRequest::isProgressMeterSwitchOff()
{
  return mIsProgressMeterSwitchOff;
}

void rtFileDownloadRequest::setHTTPFailOnError(bool val)
{
  mHTTPFailOnError = val;
}

bool rtFileDownloadRequest::isHTTPFailOnError()
{
  return mHTTPFailOnError;
}

void rtFileDownloadRequest::setHTTPError(const char* httpError)
{
  if(httpError != NULL)
  {
    strncpy(mHttpErrorBuffer, httpError, CURL_ERROR_SIZE-1);
    mHttpErrorBuffer[CURL_ERROR_SIZE-1] = '\0';
  }
}

char* rtFileDownloadRequest::httpErrorBuffer(void)
{
  return mHttpErrorBuffer;
}

void rtFileDownloadRequest::setCurlDefaultTimeout(bool val)
{
  mDefaultTimeout = val;
}

bool rtFileDownloadRequest::isCurlDefaultTimeoutSet()
{
  return mDefaultTimeout;
}

void rtFileDownloadRequest::setConnectionTimeout(long val)
{
  mConnectionTimeout = val;
}

long rtFileDownloadRequest::getConnectionTimeout()
{
  return mConnectionTimeout;
}

void rtFileDownloadRequest::setCORS(const rtCORSRef& cors)
{
  mCORS = cors;
}

rtCORSRef rtFileDownloadRequest::cors() const
{
  return mCORS;
}

void rtFileDownloadRequest::cancelRequest()
{
  mCanceledMutex.lock();
  mCanceled = true;
  mCanceledMutex.unlock();
}

bool rtFileDownloadRequest::isCanceled()
{
  bool requestCanceled = false;
  mCanceledMutex.lock();
  requestCanceled = mCanceled;
  mCanceledMutex.unlock();
  return requestCanceled;
}

void rtFileDownloadRequest::setMethod(const char* method)
{
  mMethod = method;
}

rtString rtFileDownloadRequest::method() const
{
  return mMethod;
}

void rtFileDownloadRequest::setReadData(const uint8_t* data, size_t size)
{
  mReadData = data;
  mReadDataSize = size;
}

const uint8_t* rtFileDownloadRequest::readData() const
{
  return mReadData;
}

size_t rtFileDownloadRequest::readDataSize() const
{
  return mReadDataSize;
}

void rtFileDownloadRequest::enableDownloadMetrics(bool enableDownloadMetrics)
{
  mDownloadMetricsEnabled = enableDownloadMetrics;
}

bool rtFileDownloadRequest::isDownloadMetricsEnabled(void)
{
  return mDownloadMetricsEnabled;
}

rtObjectRef rtFileDownloadRequest::downloadMetrics() const
{
  return mDownloadMetrics;
}

void rtFileDownloadRequest::setDownloadMetrics(int32_t connectTimeMs, int32_t sslConnectTimeMs, int32_t totalTimeMs, int32_t downloadSpeedBytesPerSecond)
{
  mDownloadMetrics.set("connectTimeMs", connectTimeMs);
  mDownloadMetrics.set("sslConnectTimeMs", sslConnectTimeMs);
  mDownloadMetrics.set("totalDownloadTimeMs", totalTimeMs);
  mDownloadMetrics.set("downloadSpeedBytesPerSecond", downloadSpeedBytesPerSecond);
}

void rtFileDownloadRequest::setActualFileSize(size_t actualFileSize)
{
  mActualFileSize = actualFileSize;
}

size_t rtFileDownloadRequest::actualFileSize(void)
{
  return mActualFileSize;
}

void rtFileDownloadRequest::setByteRangeEnable(bool bByteRangeFlag)
{
  mIsByteRangeEnabled = bByteRangeFlag;
}

bool rtFileDownloadRequest::isByteRangeEnabled(void)
{
  return mIsByteRangeEnabled;
}

void rtFileDownloadRequest::setByteRangeIntervals(size_t byteRangeIntervals )
{
  mByteRangeIntervals = byteRangeIntervals;
}

size_t rtFileDownloadRequest::byteRangeIntervals(void)
{
  return mByteRangeIntervals;
}

void rtFileDownloadRequest::setCurlErrRetryCount(unsigned int curlRetryCount)
{
  curlErrRetryCount = curlRetryCount;
}

unsigned int rtFileDownloadRequest::getCurlErrRetryCount(void)
{
  return curlErrRetryCount;
}

void rtFileDownloadRequest::setCurlRetryEnable(bool bCurlRetry)
{
  mCurlRetry = bCurlRetry;
}

bool rtFileDownloadRequest::isCurlRetryEnabled(void)
{
  return mCurlRetry;
}

void rtFileDownloadRequest::setDownloadOnly(bool downloadOnly)
{
  mDownloadOnly = downloadOnly;
}

bool rtFileDownloadRequest::isDownloadOnly(void)
{
  return mDownloadOnly;
}

void rtFileDownloadRequest::setUseEncoding(bool useEncoding)
{
  mUseEncoding = useEncoding;
}

bool rtFileDownloadRequest::isUseEncoding() const
{
  return mUseEncoding;
}

void rtFileDownloadRequest::setUserAgent(const char* userAgent)
{
  mUserAgent = userAgent;
}

rtString rtFileDownloadRequest::userAgent() const
{
  return mUserAgent;
}

void rtFileDownloadRequest::setRedirectFollowLocation(bool redirectFollowLocation)
{
  mRedirectFollowLocation = redirectFollowLocation;
}

bool rtFileDownloadRequest::isRedirectFollowLocationEnabled(void)
{
  return mRedirectFollowLocation;
}

void rtFileDownloadRequest::setKeepTCPAlive(bool keepTCPAlive)
{
  mKeepTCPAlive = keepTCPAlive;
}

bool rtFileDownloadRequest::keepTCPAliveEnabled(void)
{
  return mKeepTCPAlive;
}

void rtFileDownloadRequest::setCROSRequired(bool crosRequired)
{
  mCROSRequired = crosRequired;
}

bool rtFileDownloadRequest::isCROSRequired(void)
{
  return mCROSRequired;
}

rtFileDownloader::rtFileDownloader()
    : mNumberOfCurrentDownloads(0), mDefaultCallbackFunction(NULL), mDownloadHandles(), mReuseDownloadHandles(false),
      mCaCertFile(CA_CERTIFICATE), mFileCacheMutex()
{
  CURLcode rv = curl_global_init(CURL_GLOBAL_ALL);
  if (CURLE_OK != rv)
  {
    rtLogError("curl global init failed (error code: %d)", rv);
  }
#ifdef PX_REUSE_DOWNLOAD_HANDLES
  downloadHandleMutex.lock();
  int numberOfDownloadHandles = rtThreadPool::globalInstance()->numberOfThreadsInPool();
  rtLogWarn("enabling curl handle reuse with pool size of: %d", numberOfDownloadHandles);
  for (int i = 0; i < numberOfDownloadHandles; i++)
  {
    mDownloadHandles.push_back(rtFileDownloadHandle(curl_easy_init()));
  }
  mReuseDownloadHandles = true;
  downloadHandleMutex.unlock();
#endif
  char const* s = getenv("CA_CERTIFICATE_FILE");
  if (s)
  {
    mCaCertFile = s;
  }
}

rtFileDownloader::~rtFileDownloader()
{
#ifdef PX_REUSE_DOWNLOAD_HANDLES
  downloadHandleMutex.lock();
  for (vector<rtFileDownloadHandle>::iterator it = mDownloadHandles.begin(); it != mDownloadHandles.end(); )
  {
    CURL *curlHandle = (*it).curlHandle;
    if (curlHandle != NULL)
    {
      curl_easy_cleanup(curlHandle);
    }
    it = mDownloadHandles.erase(it);
  }
  mReuseDownloadHandles = false;
  downloadHandleMutex.unlock();
  if (rtFileDownloader::instance() == this)
  {
    //cleanup curl and shutdown the reuse handle thread if this is the singleton object
    downloadHandleMutex.lock();
    continueDownloadHandleCheck = false;
    downloadHandleMutex.unlock();
    if (downloadHandleExpiresCheckThread)
    {
      rtLogDebug("close thread and wait");
      downloadHandleExpiresCheckThread->join();
      rtLogDebug("done with join");
      delete downloadHandleExpiresCheckThread;
      downloadHandleExpiresCheckThread = NULL;
    }
  }
#endif
  mCaCertFile = "";
}

rtFileDownloader* rtFileDownloader::instance()
{
    if (mInstance == NULL)
    {
#ifndef WIN32
        signal(SIGPIPE, SIG_IGN);
#endif //!WIN32
        mInstance = new rtFileDownloader();
#ifdef PX_REUSE_DOWNLOAD_HANDLES
      downloadHandleExpiresCheckThread = new std::thread(onDownloadHandleCheck);
#endif //PX_REUSE_DOWNLOAD_HANDLES
    }
    return mInstance;
}

void rtFileDownloader::deleteInstance()
{
    if (mInstance != NULL)
    {
        delete mInstance;
        mInstance = NULL;
    }
}

bool rtFileDownloader::addToDownloadQueue(rtFileDownloadRequest* downloadRequest)
{
    bool submitted = false;
    //todo: check the download queue before starting download
    submitted = true;
    addFileDownloadRequest(downloadRequest);
    downloadFileInBackground(downloadRequest);
    //startNextDownloadInBackground();
    return submitted;
}

void rtFileDownloader::startNextDownloadInBackground()
{
    //todo
}

void rtFileDownloader::raiseDownloadPriority(rtFileDownloadRequest* downloadRequest)
{
  if (downloadRequest != NULL)
  {
    rtThreadPool *mainThreadPool = rtThreadPool::globalInstance();
    mainThreadPool->raisePriority(downloadRequest->fileUrl());
  }
}

void rtFileDownloader::removeDownloadRequest(rtFileDownloadRequest* downloadRequest)
{
    (void)downloadRequest;
    //todo
}

void rtFileDownloader::clearFileCache()
{
    //todo
}

void rtFileDownloader::downloadFileAsByteRange(rtFileDownloadRequest* downloadRequest)
{
  bool isRequestCanceled = downloadRequest->isCanceled();
  if (isRequestCanceled)
  {
    downloadRequest->setDownloadStatusCode(HTTP_DOWNLOAD_CANCELED);
    downloadRequest->setDownloadedData(NULL, 0);
    downloadRequest->setDownloadStatusCode(-1);
    downloadRequest->setErrorString("canceled request");
    if (!downloadRequest->executeCallback(downloadRequest->downloadStatusCode()))
    {
      if (mDefaultCallbackFunction != NULL)
      {
        (*mDefaultCallbackFunction)(downloadRequest);
      }
    }
    clearFileDownloadRequest(downloadRequest);
    return;
  }

#ifdef ENABLE_HTTP_CACHE
    bool isDataInCache = false;
#endif
    bool nwDownloadSuccess = false;

#ifdef ENABLE_HTTP_CACHE
    rtHttpCacheData cachedData(downloadRequest->fileUrl().cString());
    if (true == downloadRequest->cacheEnabled())
    {
      if (true == checkAndDownloadFromCache(downloadRequest,cachedData))
      {
        isDataInCache = true;
        downloadRequest->setDataIsCached(true);
      }
    }

    if (isDataInCache)
    {
      if(downloadRequest->isDownloadOnly() == false)
      {
        if(downloadRequest->deferCacheRead())
        {
            rtLogInfo("Reading from cache Start for %s\n", downloadRequest->fileUrl().cString());
            FILE *fp = downloadRequest->cacheFilePointer();

            if(fp != NULL)
            {
                char* buffer = new char[downloadRequest->getCachedFileReadSize()];
                size_t bytesCount = 0;
                size_t dataSize = 0;
                char invalidData[8] = "Invalid";

                // The cahced file has expiration value ends with | delimeter.
                while ( !feof(fp) )
                {
                    dataSize++;
                    if (fgetc(fp) == '|')
                        break;
                }
                while (!feof(fp))
                {
                    memset(buffer, 0, downloadRequest->getCachedFileReadSize());
                    bytesCount = fread(buffer, 1, downloadRequest->getCachedFileReadSize(), fp);
                    dataSize += bytesCount;
                    downloadRequest->executeExternalWriteCallback((unsigned char*)buffer, bytesCount, 1 );
                }
                // For deferCacheRead, the user requires the downloadedDataSize but not the data.
                downloadRequest->setDownloadedData( invalidData, dataSize);
                delete [] buffer;
                fclose(fp);
            }
            rtLogInfo("Reading from cache End for %s\n", downloadRequest->fileUrl().cString());
        }
      }
    }
    else
#endif
    {
      const rtString actualUrl = downloadRequest->fileUrl();
      bool reDirect = false;
      nwDownloadSuccess = downloadByteRangeFromNetwork(downloadRequest, &reDirect);
      if(reDirect == true)
      {
         reDirect = false;
         nwDownloadSuccess = downloadByteRangeFromNetwork(downloadRequest, &reDirect);
         downloadRequest->setFileUrl(actualUrl.cString());
      }
    }

    if (!downloadRequest->executeCallback(downloadRequest->downloadStatusCode()))
    {
      if (mDefaultCallbackFunction != NULL)
      {
        (*mDefaultCallbackFunction)(downloadRequest);
      }
    }

#ifdef ENABLE_HTTP_CACHE
    // Store the network data in cache
    if ((true == nwDownloadSuccess) &&
        (true == downloadRequest->cacheEnabled())  &&
        (downloadRequest->actualFileSize() == downloadRequest->downloadedDataSize()) &&
        (downloadRequest->httpStatusCode() != 302) &&
        (downloadRequest->httpStatusCode() != 307))
    {
      rtHttpCacheData downloadedData(downloadRequest->fileUrl(),
                                     downloadRequest->headerData(),
                                     downloadRequest->downloadedData(),
                                     downloadRequest->downloadedDataSize());

      if (downloadedData.isWritableToCache())
      {
        mFileCacheMutex.lock();
        if (NULL == rtFileCache::instance())
          rtLogWarn("cache data not added");
        else
        {
          rtFileCache::instance()->addToCache(downloadedData);
        }
        mFileCacheMutex.unlock();
      }
    }

    // Store the updated data in cache
    if ((true == isDataInCache) && (cachedData.isUpdated()) && (downloadRequest->isDownloadOnly() == false))
    {
      rtString url;
      cachedData.url(url);

      mFileCacheMutex.lock();
      if (NULL == rtFileCache::instance())
          rtLogWarn("Adding url to cache failed (%s) due to in-process memory issues", url.cString());
      rtFileCache::instance()->removeData(url);
      mFileCacheMutex.unlock();
      if (cachedData.isWritableToCache())
      {
        mFileCacheMutex.lock();
        rtError err = rtFileCache::instance()->addToCache(cachedData);
        if (RT_OK != err)
          rtLogWarn("Adding url to cache failed (%s)", url.cString());

        mFileCacheMutex.unlock();
      }
    }

    if (true == isDataInCache)
    {
      downloadRequest->setHeaderData(NULL,0);
      downloadRequest->setDownloadedData(NULL,0);
    }
#endif
    clearFileDownloadRequest(downloadRequest);
}

bool rtFileDownloader::downloadByteRangeFromNetwork(rtFileDownloadRequest* downloadRequest, bool *bRedirect)
{
   CURL *curl_handle = NULL;
   CURLcode res = CURLE_OK;
   char errorBuffer[CURL_ERROR_SIZE];
   size_t multipleIntervals = 1;
   size_t startRange = 0;
   rtString byteRange("NULL");
   rtString strLocation;
   unsigned int curlErrRetryCount = 0;
   rtString curlUrl = downloadRequest->fileUrl();
   bool useProxy = !downloadRequest->proxy().isEmpty();
   rtString proxyServer = downloadRequest->proxy();
   bool headerOnly = downloadRequest->headerOnly();
   MemoryStruct chunk;
   double totalTimeTaken = 0;

   rtString method = downloadRequest->method();
   size_t readDataSize = downloadRequest->readDataSize();

   rtString origin = rtUrlGetOrigin(downloadRequest->fileUrl());

   curl_handle = rtFileDownloader::instance()->retrieveDownloadHandle(origin);
   curl_easy_reset(curl_handle);
   /* specify URL to get */
   curl_easy_setopt(curl_handle, CURLOPT_URL, curlUrl.cString());
   if(downloadRequest->isRedirectFollowLocationEnabled())
      curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1); //when redirected, follow the redirections
   curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, HeaderCallback);
   curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&chunk);
   if (false == headerOnly)
   {
      chunk.downloadRequest = downloadRequest;
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
   }

   if(downloadRequest->isCurlDefaultTimeoutSet() == false)
   {
      curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, kCurlTimeoutInSeconds);
   }
   if(downloadRequest->getConnectionTimeout() != 0)
   {
      curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, downloadRequest->getConnectionTimeout());
   }

   curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);

   if(downloadRequest->isProgressMeterSwitchOff())
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1);
   else
   {
      if(downloadRequest->progressCallback())
      {
         curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, downloadRequest->progressCallback());
         curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, downloadRequest->progressCallbackUserPtr());
         curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0);
      }
   }

   if(downloadRequest->isHTTPFailOnError())
   {
      memset(errorBuffer, 0, sizeof(errorBuffer));
      curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1);
      curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1);
      curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, errorBuffer);
   }
#if !defined(PX_PLATFORM_GENERIC_DFB) && !defined(PX_PLATFORM_DFB_NON_X11)
   if(downloadRequest->keepTCPAliveEnabled())
   {
      curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1);
      curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPIDLE, 60);
      curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPINTVL, 30);
   }
#endif //!PX_PLATFORM_GENERIC_DFB && !PX_PLATFORM_DFB_NON_X11

   double downloadHandleExpiresTime = downloadRequest->downloadHandleExpiresTime();

   struct curl_slist *list = NULL;
   if(downloadRequest->isCROSRequired())
   {
      vector<rtString>& additionalHttpHeaders = downloadRequest->additionalHttpHeaders();
      struct curl_slist *list = NULL;
      for (unsigned int headerOption = 0;headerOption < additionalHttpHeaders.size();headerOption++)
      {
         list = curl_slist_append(list, additionalHttpHeaders[headerOption].cString());
      }
      if (downloadRequest->cors() != NULL)
         downloadRequest->cors()->updateRequestForAccessControl(&list);
   }
   if (readDataSize > 0)
   {
      list = curl_slist_append(list, "Expect:");
   }
   if(list)
      curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);

   //CA certificates
   // !CLF: Use system CA Cert rather than CA_CERTIFICATE fo now.  Revisit!
   //curl_easy_setopt(curl_handle,CURLOPT_CAINFO,mCaCertFile.cString());
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, true);

   /* some servers don't like requests that are made without a user-agent
      field, so we provide one */
   if(!downloadRequest->userAgent().isEmpty())
   {
      curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, downloadRequest->userAgent().cString());
   }
   else
   {
      curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
   }

   if (useProxy)
   {
      curl_easy_setopt(curl_handle, CURLOPT_PROXY, proxyServer.cString());
      curl_easy_setopt(curl_handle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
   }
   else
   {
      curl_easy_setopt(curl_handle, CURLOPT_PROXY, "");
   }

   if (true == headerOnly)
   {
      curl_easy_setopt(curl_handle, CURLOPT_NOBODY, 1);
   }

   if (!method.isEmpty() && method.compare("GET") != 0)
   {
      if (method.compare("POST") == 0)
         curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
      else if (method.compare("PUT") == 0)
         curl_easy_setopt(curl_handle, CURLOPT_UPLOAD, 1L);
      else
         curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, method.cString());
   }

   if (readDataSize > 0)
   {
      chunk.downloadRequest = downloadRequest;
      curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, ReadMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_READDATA, (void *)&chunk);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, readDataSize);
   }

   for(int iLoop=0; iLoop<= multipleIntervals; /* Increment handled inside the body. *iamhere */)
   {
      if(curlErrRetryCount < downloadRequest->getCurlErrRetryCount())
      {
         if(iLoop < multipleIntervals)
         {
            // Request for (0  to 8k) bytes to get http header, once it got success request for (8k+1 to BYTE_RANGE_SPLIT-1)bytes.
            // Then there onwards request for BYTE_RANGE_SPLIT bytes until the end.
            if(iLoop == 0)
            {
               byteRange = rtString::toString(0) + "-" + rtString::toString(8192-1);
            }
            else if(iLoop == 1)
            {
               startRange = 0;
               byteRange = rtString::toString(8192) + "-" + rtString::toString(startRange + downloadRequest->byteRangeIntervals()-1);
            }
            else
            {
               byteRange = rtString::toString(startRange) + "-" + rtString::toString(startRange + downloadRequest->byteRangeIntervals()-1);
            }
         }
         else if(iLoop == multipleIntervals)
         {
            if(downloadRequest->actualFileSize() % downloadRequest->byteRangeIntervals())
            {
               byteRange = rtString::toString(startRange) + "-" + rtString::toString(downloadRequest->actualFileSize()-1);
            }
         }
         curl_easy_setopt(curl_handle, CURLOPT_RANGE, byteRange.cString());
      }

      res = curl_easy_perform(curl_handle);
      if(res != CURLE_OK)
      {
         if(res == CURLE_COULDNT_CONNECT)
         {
            if(downloadRequest->isCurlRetryEnabled())
            {
               // If there is above metioned error even after retry, quit from curl download.
               if(curlErrRetryCount == downloadRequest->getCurlErrRetryCount())
               {
                  rtLogInfo("After retry retryCount(%d). Error(%d) occured during curl download for byteRange(%s) Url (%s)", curlErrRetryCount, res, byteRange.cString(), curlUrl.cString());
                  break;
               }

               // If there is above mentioned error, retry one more time. Don't increment, execute the loop for the same index.
               curlErrRetryCount++;
               rtLogInfo("Retry again retryCount(%d). Error(%d) occured during curl download for byteRange[%s] Url (%s)", curlErrRetryCount, res, byteRange.cString(), curlUrl.cString());
               continue;
            }
         }
         rtLogInfo("Error(%d) occured during curl download for byteRange(%s) Url (%s)", res, byteRange.cString(), curlUrl.cString());
         break;
      }
      else
      {
         if(CURLE_OK == curl_easy_getinfo(curl_handle, CURLINFO_TOTAL_TIME, &totalTimeTaken))
         {
            rtLogInfo("Time taken to complete curl_easy_perform is %.1f sec for byterange (%s) url (%s)\n", totalTimeTaken, byteRange.cString(), curlUrl.cString() );
         }
      }

      if((iLoop == 0) && (res == CURLE_OK))
      {
         curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, NULL);
         curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, NULL);

         rtString httpHeaderStr(reinterpret_cast< char const* >((unsigned char *)chunk.headerBuffer));

         // Parsing redirected Url if 302 found.
         size_t find302Status = httpHeaderStr.find(0, "302 Found");
         if(find302Status != -1)
         {
            rtString str302Found = httpHeaderStr.substring(find302Status);
            size_t findReDirLoc = str302Found.find(0, "Location:");

            if(findReDirLoc != -1)
            {
               strLocation = str302Found.substring(findReDirLoc+strlen("Location:"));
               strLocation = strLocation.substring(0, strLocation.find(0, "\n")-1);
               curlUrl = strLocation.trim();
               rtLogInfo("302 Found. Redirected curl URL (%s)", curlUrl.cString());

               //clean up contents on error
               if (chunk.contentsBuffer != NULL)
               {
                  free(chunk.contentsBuffer);
                  chunk.contentsBuffer = NULL;
                  chunk.contentsSize = 0;
               }

               if (chunk.headerBuffer != NULL)
               {
                  free(chunk.headerBuffer);
                  chunk.headerBuffer = NULL;
                  chunk.headerSize = 0;
               }
               downloadRequest->setDownloadedData(NULL, 0);
               downloadRequest->setHeaderData(NULL, 0);

               downloadRequest->setFileUrl(curlUrl.cString());
               rtLogInfo("downloadRequest->fileUrl().cString() URL (%s)", downloadRequest->fileUrl().cString());
               *bRedirect = true;
               break;
            }
         }

         // Parsing total filesize from Content-Range.
         int32_t findContentRange = httpHeaderStr.find(0, "Content-Range: bytes");
         if(findContentRange != -1)
         {
            rtString strContentRange = httpHeaderStr.substring(findContentRange+1);
            int32_t findRange = strContentRange.find(0, "/");

            if(findRange != -1)
            {
               rtString substrRange = strContentRange.substring(findRange+1);
               rtString strRange = substrRange.substring(0, substrRange.find(0, "\n")-1);
               downloadRequest->setActualFileSize(atoi(strRange.cString()));
               curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0);
               rtLogInfo("FileSize (%ld) from http header(Content-Range). Url(%s)\n", downloadRequest->actualFileSize(), downloadRequest->fileUrl().cString());
            }
            else
               rtLogError("Http header Content-Range is not in xxx/yyy format. Url(%s)\n", downloadRequest->fileUrl().cString());
         }
         else
            rtLogError("Http header doesn't have Content-Range. Url(%s)\n", downloadRequest->fileUrl().cString());

         multipleIntervals = (downloadRequest->actualFileSize() >= downloadRequest->byteRangeIntervals()) ? (downloadRequest->actualFileSize() / downloadRequest->byteRangeIntervals()) : 0;
         multipleIntervals++; // Increment one more because already one iteration made before determinding multipleIntervals.
         rtLogInfo("File[%s] multipleIntervals [%ld] fileSize[%ld]\n", downloadRequest->fileUrl().cString(), multipleIntervals, downloadRequest->actualFileSize());
      }
      startRange += downloadRequest->byteRangeIntervals();

      curlErrRetryCount = 0;
      iLoop++; // *iamhere
   }

   if(list)
      curl_slist_free_all(list);

   downloadRequest->setDownloadStatusCode(res);
   if(downloadRequest->isHTTPFailOnError())
      downloadRequest->setHTTPError(errorBuffer);

   /* check for errors */
   if (res != CURLE_OK)
   {
      rtString proxyMessage("Using proxy:");
      if (useProxy)
      {
         proxyMessage.append("true - ");
         proxyMessage.append(proxyServer.cString());
      }
      else
      {
         proxyMessage.append("false ");
      }
      char errorMessage[MAX_URL_SIZE+400];
      memset(errorMessage, 0, sizeof(errorMessage));
      sprintf(errorMessage, "Download error for:%s. Error code:%d. %s",downloadRequest->fileUrl().cString(), res, proxyMessage.cString());
      downloadRequest->setErrorString(errorMessage);
      rtFileDownloader::instance()->releaseDownloadHandle(curl_handle, downloadHandleExpiresTime, origin);

      //clean up contents on error
      if (chunk.contentsBuffer != NULL)
      {
         free(chunk.contentsBuffer);
         chunk.contentsBuffer = NULL;
      }

      if (chunk.headerBuffer != NULL)
      {
         free(chunk.headerBuffer);
         chunk.headerBuffer = NULL;
      }
      downloadRequest->setDownloadedData(NULL, 0);
      return false;
   }

   if(downloadRequest->isDownloadMetricsEnabled())
   {
      // record download stats
      double connectTime = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_CONNECT_TIME, &connectTime);
      double sslConnectTime = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_APPCONNECT_TIME, &sslConnectTime);
      double downloadSpeed = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_SPEED_DOWNLOAD, &downloadSpeed);
      double totalDownloadTime = 0;
      curl_easy_getinfo(curl_handle, CURLINFO_TOTAL_TIME, &totalDownloadTime);

      if (sslConnectTime < connectTime)
      {
         sslConnectTime = connectTime;
      }

      rtLogInfo("download stats - connect time: %d ms, ssl time: %d ms, total time: %d ms, download speed: %d bytes/sec, url: %s",
              (int)(connectTime*1000), (int)((sslConnectTime - connectTime) * 1000),
              (int)(totalDownloadTime*1000), (int)downloadSpeed, downloadRequest->fileUrl().cString());

      downloadRequest->setDownloadMetrics(static_cast<int32_t>(connectTime*1000), static_cast<int32_t>((sslConnectTime - connectTime) * 1000),
                                        static_cast<int32_t>(totalDownloadTime*1000), static_cast<int32_t>(downloadSpeed));
   }

   long httpCode = -1;
   if (curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpCode) == CURLE_OK)
   {
      downloadRequest->setHttpStatusCode(httpCode);
   }
   rtFileDownloader::instance()->releaseDownloadHandle(curl_handle, downloadHandleExpiresTime, origin);

   //todo read the header information before closing
   if (chunk.headerBuffer != NULL)
   {
      downloadRequest->setHeaderData(chunk.headerBuffer, chunk.headerSize);
   }

   //don't free the downloaded data (contentsBuffer) because it will be used later
   if (false == headerOnly)
   {
     downloadRequest->setDownloadedData(chunk.contentsBuffer, chunk.contentsSize);
   }
   else if (chunk.contentsBuffer != NULL)
   {
      free(chunk.contentsBuffer);
      chunk.contentsBuffer = NULL;
   }
   chunk.headerBuffer = NULL;
   chunk.contentsBuffer = NULL;
   if(downloadRequest->isCROSRequired())
   {
      if (downloadRequest->cors() != NULL)
         downloadRequest->cors()->updateResponseForAccessControl(downloadRequest);
   }
   return true;
}

void rtFileDownloader::downloadFile(rtFileDownloadRequest* downloadRequest)
{
  bool isRequestCanceled = downloadRequest->isCanceled();
  if (isRequestCanceled)
  {
    downloadRequest->setDownloadStatusCode(HTTP_DOWNLOAD_CANCELED);
    downloadRequest->setDownloadedData(NULL, 0);
    downloadRequest->setDownloadStatusCode(-1);
    downloadRequest->setErrorString("canceled request");
    if (!downloadRequest->executeCallback(downloadRequest->downloadStatusCode()))
    {
      if (mDefaultCallbackFunction != NULL)
      {
        (*mDefaultCallbackFunction)(downloadRequest);
      }
    }
    clearFileDownloadRequest(downloadRequest);
    return;
  }

#ifdef ENABLE_HTTP_CACHE
    bool isDataInCache = false;
#endif
    bool nwDownloadSuccess = false;

#ifdef ENABLE_HTTP_CACHE
    rtHttpCacheData cachedData(downloadRequest->fileUrl().cString());
    if (true == downloadRequest->cacheEnabled())
    {
      if (true == checkAndDownloadFromCache(downloadRequest,cachedData))
      {
        isDataInCache = true;
        downloadRequest->setDataIsCached(true);
      }
    }

    if (isDataInCache)
    {
        if(downloadRequest->deferCacheRead())
        {
            rtLogInfo("Reading from cache Start for %s\n", downloadRequest->fileUrl().cString());
            FILE *fp = downloadRequest->cacheFilePointer();

            if(fp != NULL)
            {
                char* buffer = new char[downloadRequest->getCachedFileReadSize()];
                size_t bytesCount = 0;
                size_t dataSize = 0;                
				char invalidData[8] = "Invalid";

                // The cahced file has expiration value ends with | delimeter.
                while ( !feof(fp) )
                {
                    dataSize++;
                    if (fgetc(fp) == '|')
                        break;
                }
                while (!feof(fp))
                {
                    memset(buffer, 0, downloadRequest->getCachedFileReadSize());
                    bytesCount = fread(buffer, 1, downloadRequest->getCachedFileReadSize(), fp);
                    dataSize += bytesCount;
                    downloadRequest->executeExternalWriteCallback((unsigned char*)buffer, bytesCount, 1 );
                }
                // For deferCacheRead, the user requires the downloadedDataSize but not the data.
                downloadRequest->setDownloadedData( invalidData, dataSize);
                delete [] buffer;
                fclose(fp);
            }
            rtLogInfo("Reading from cache End for %s\n", downloadRequest->fileUrl().cString());
        }
    }
    else
#endif
    {
      nwDownloadSuccess = downloadFromNetwork(downloadRequest);
    }    
    
    if (!downloadRequest->executeCallback(downloadRequest->downloadStatusCode()))
    {
      if (mDefaultCallbackFunction != NULL)
      {
        (*mDefaultCallbackFunction)(downloadRequest);
      }
    }

#ifdef ENABLE_HTTP_CACHE
    // Store the network data in cache
    if ((true == nwDownloadSuccess) &&
        (true == downloadRequest->cacheEnabled())  &&
        (downloadRequest->httpStatusCode() != 206) &&
        (downloadRequest->httpStatusCode() != 302) &&
        (downloadRequest->httpStatusCode() != 307))
    {
      rtHttpCacheData downloadedData(downloadRequest->fileUrl(),
                                     downloadRequest->headerData(),
                                     downloadRequest->downloadedData(),
                                     downloadRequest->downloadedDataSize());

      if (downloadedData.isWritableToCache())
      {
        mFileCacheMutex.lock();
        if (NULL == rtFileCache::instance())
          rtLogWarn("cache data not added");
        else
        {
          rtFileCache::instance()->addToCache(downloadedData);
        }
        mFileCacheMutex.unlock();
      }
    }

    // Store the updated data in cache
    if ((true == isDataInCache) && (cachedData.isUpdated()))
    {
      rtString url;
      cachedData.url(url);

      mFileCacheMutex.lock();
      if (NULL == rtFileCache::instance())
          rtLogWarn("Adding url to cache failed (%s) due to in-process memory issues", url.cString());
      rtFileCache::instance()->removeData(url);
      mFileCacheMutex.unlock();
      if (cachedData.isWritableToCache())
      {
        mFileCacheMutex.lock();
        rtError err = rtFileCache::instance()->addToCache(cachedData);
        if (RT_OK != err)
          rtLogWarn("Adding url to cache failed (%s)", url.cString());
        
        mFileCacheMutex.unlock();
      }
    }

    if (true == isDataInCache)
    {
      downloadRequest->setHeaderData(NULL,0);
      downloadRequest->setDownloadedData(NULL,0);
    }
#endif
    clearFileDownloadRequest(downloadRequest);
}

bool rtFileDownloader::downloadFromNetwork(rtFileDownloadRequest* downloadRequest)
{
    CURL *curl_handle = NULL;
    CURLcode res = CURLE_OK;
    char errorBuffer[CURL_ERROR_SIZE];

    bool useProxy = !downloadRequest->proxy().isEmpty();
    rtString proxyServer = downloadRequest->proxy();
    bool headerOnly = downloadRequest->headerOnly();
    MemoryStruct chunk;

    rtString method = downloadRequest->method();
    size_t readDataSize = downloadRequest->readDataSize();

    rtString origin = rtUrlGetOrigin(downloadRequest->fileUrl());

    curl_handle = rtFileDownloader::instance()->retrieveDownloadHandle(origin);
    curl_easy_reset(curl_handle);
    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, downloadRequest->fileUrl().cString());
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1); //when redirected, follow the redirections
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&chunk);
    if (false == headerOnly)
    {
      chunk.downloadRequest = downloadRequest;
      curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    }

    if(downloadRequest->isCurlDefaultTimeoutSet() == false)
    {
      curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, kCurlTimeoutInSeconds);
    }
    curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);

    if(downloadRequest->isProgressMeterSwitchOff())
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1);
	else
	{
      if(downloadRequest->progressCallback())
      {
         curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, downloadRequest->progressCallback());
         curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, downloadRequest->progressCallbackUserPtr());
         curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0);
      }
   }

    if(downloadRequest->isHTTPFailOnError())
    {
        memset(errorBuffer, 0, sizeof(errorBuffer));
        curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1);
        curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, errorBuffer);
    }
#if !defined(PX_PLATFORM_GENERIC_DFB) && !defined(PX_PLATFORM_DFB_NON_X11)
    curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPIDLE, 60);
    curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPINTVL, 30);
#endif //!PX_PLATFORM_GENERIC_DFB && !PX_PLATFORM_DFB_NON_X11

    double downloadHandleExpiresTime = downloadRequest->downloadHandleExpiresTime();

    vector<rtString>& additionalHttpHeaders = downloadRequest->additionalHttpHeaders();
    struct curl_slist *list = NULL;
    for (unsigned int headerOption = 0;headerOption < additionalHttpHeaders.size();headerOption++)
    {
      list = curl_slist_append(list, additionalHttpHeaders[headerOption].cString());
    }
    if (downloadRequest->cors() != NULL)
      downloadRequest->cors()->updateRequestForAccessControl(&list);
    if (readDataSize > 0)
    {
      list = curl_slist_append(list, "Expect:");
    }
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);
    //CA certificates
    // !CLF: Use system CA Cert rather than CA_CERTIFICATE fo now.  Revisit!
    //curl_easy_setopt(curl_handle,CURLOPT_CAINFO,mCaCertFile.cString());
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, true);

    /* some servers don't like requests that are made without a user-agent
     field, so we provide one */
    if(!downloadRequest->userAgent().isEmpty())
    {
       curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, downloadRequest->userAgent().cString());
    }
    else
    {
       curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
    }

    if (useProxy)

    {
        curl_easy_setopt(curl_handle, CURLOPT_PROXY, proxyServer.cString());
        curl_easy_setopt(curl_handle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }
    else
    {
      curl_easy_setopt(curl_handle, CURLOPT_PROXY, "");
    }

    if (true == headerOnly)
    {
      curl_easy_setopt(curl_handle, CURLOPT_NOBODY, 1);
    }

    if (!method.isEmpty() && method.compare("GET") != 0)
    {
      if (method.compare("POST") == 0)
        curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
      else if (method.compare("PUT") == 0)
        curl_easy_setopt(curl_handle, CURLOPT_UPLOAD, 1L);
      else
        curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, method.cString());
    }

    if (readDataSize > 0)
    {
      chunk.downloadRequest = downloadRequest;
      curl_easy_setopt(curl_handle, CURLOPT_READFUNCTION, ReadMemoryCallback);
      curl_easy_setopt(curl_handle, CURLOPT_READDATA, (void *)&chunk);
      curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, readDataSize);
    }

    if (downloadRequest->isUseEncoding())
    {
      /* enable all supported built-in compressions */
      curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");
    }

    /* get it! */
    res = curl_easy_perform(curl_handle);
    curl_slist_free_all(list);

    downloadRequest->setDownloadStatusCode(res);
    if(downloadRequest->isHTTPFailOnError())
        downloadRequest->setHTTPError(errorBuffer);

    /* check for errors */
    if (res != CURLE_OK)
    {
        rtString proxyMessage("Using proxy:");
        if (useProxy)
        {
          proxyMessage.append("true - ");
          proxyMessage.append(proxyServer.cString());
        }
        else
        {
          proxyMessage.append("false ");
        }
        char errorMessage[MAX_URL_SIZE+400];
        memset(errorMessage, 0, sizeof(errorMessage));
        sprintf(errorMessage, "Download error for:%s. Error code:%d. %s",downloadRequest->fileUrl().cString(), res, proxyMessage.cString());
        downloadRequest->setErrorString(errorMessage);
        rtFileDownloader::instance()->releaseDownloadHandle(curl_handle, downloadHandleExpiresTime, origin);

        //clean up contents on error
        if (chunk.contentsBuffer != NULL)
        {
            free(chunk.contentsBuffer);
            chunk.contentsBuffer = NULL;
        }

        if (chunk.headerBuffer != NULL)
        {
            free(chunk.headerBuffer);
            chunk.headerBuffer = NULL;
        }
        downloadRequest->setDownloadedData(NULL, 0);
        return false;
    }

    // record download stats
    double connectTime = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_CONNECT_TIME, &connectTime);
    double sslConnectTime = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_APPCONNECT_TIME, &sslConnectTime);
    double downloadSpeed = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_SPEED_DOWNLOAD, &downloadSpeed);
    double totalDownloadTime = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_TOTAL_TIME, &totalDownloadTime);

    if (sslConnectTime < connectTime)
    {
      sslConnectTime = connectTime;
    }

    rtLogInfo("download stats - connect time: %d ms, ssl time: %d ms, total time: %d ms, download speed: %d bytes/sec, url: %s",
              (int)(connectTime*1000), (int)((sslConnectTime - connectTime) * 1000),
              (int)(totalDownloadTime*1000), (int)downloadSpeed, downloadRequest->fileUrl().cString());

    downloadRequest->setDownloadMetrics(static_cast<int32_t>(connectTime*1000), static_cast<int32_t>((sslConnectTime - connectTime) * 1000),
                                        static_cast<int32_t>(totalDownloadTime*1000), static_cast<int32_t>(downloadSpeed));

    long httpCode = -1;
    if (curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpCode) == CURLE_OK)
    {
        downloadRequest->setHttpStatusCode(httpCode);
    }
    rtFileDownloader::instance()->releaseDownloadHandle(curl_handle, downloadHandleExpiresTime, origin);

    //todo read the header information before closing
    if (chunk.headerBuffer != NULL)
    {
        downloadRequest->setHeaderData(chunk.headerBuffer, chunk.headerSize);
    }

    //don't free the downloaded data (contentsBuffer) because it will be used later
    if (false == headerOnly)
    {
      downloadRequest->setDownloadedData(chunk.contentsBuffer, chunk.contentsSize);
    }
    else if (chunk.contentsBuffer != NULL)
    {
        free(chunk.contentsBuffer);
        chunk.contentsBuffer = NULL;
    }
    chunk.headerBuffer = NULL;
    chunk.contentsBuffer = NULL;
    if (downloadRequest->cors() != NULL)
      downloadRequest->cors()->updateResponseForAccessControl(downloadRequest);
    return true;
}

#ifdef ENABLE_HTTP_CACHE
bool rtFileDownloader::checkAndDownloadFromCache(rtFileDownloadRequest* downloadRequest,rtHttpCacheData& cachedData)
{
  rtError err;
  rtData data;
  mFileCacheMutex.lock();
  if ((NULL != rtFileCache::instance()) && (RT_OK == rtFileCache::instance()->httpCacheData(downloadRequest->fileUrl(),cachedData)))
  {
    if(downloadRequest->deferCacheRead())
      err = cachedData.deferCacheRead(data);
    else
      err = cachedData.data(data);
    if (RT_OK !=  err)
    {
      mFileCacheMutex.unlock();
      return false;
    }

    downloadRequest->setHeaderData((char *)cachedData.headerData().data(),cachedData.headerData().length());
    downloadRequest->setDownloadedData((char *)cachedData.contentsData().data(),cachedData.contentsData().length());
    downloadRequest->setDownloadStatusCode(0);
    downloadRequest->setHttpStatusCode(200);
    mFileCacheMutex.unlock();
    return true;
  }
  mFileCacheMutex.unlock();
  return false;
}
#endif

void rtFileDownloader::downloadFileInBackground(rtFileDownloadRequest* downloadRequest)
{
    rtThreadPool* mainThreadPool = rtThreadPool::globalInstance();

    if (downloadRequest->downloadHandleExpiresTime() < -1)
    {
      downloadRequest->setDownloadHandleExpiresTime(kDefaultDownloadHandleExpiresTime);
    }

    rtThreadTask* task = new rtThreadTask(startFileDownloadInBackground, (void*)downloadRequest, downloadRequest->fileUrl());

    mainThreadPool->executeTask(task);
}

rtFileDownloadRequest* rtFileDownloader::nextDownloadRequest()
{
    //todo
    return NULL;
}

void rtFileDownloader::setDefaultCallbackFunction(void (*callbackFunction)(rtFileDownloadRequest*))
{
  mDefaultCallbackFunction = callbackFunction;
}

CURL* rtFileDownloader::retrieveDownloadHandle(rtString& origin)
{
  CURL* curlHandle = NULL;
#ifdef PX_REUSE_DOWNLOAD_HANDLES
  downloadHandleMutex.lock();
  if (!mReuseDownloadHandles || mDownloadHandles.empty())
  {
    curlHandle = curl_easy_init();
  }
  else
  {
    for (vector<rtFileDownloadHandle>::reverse_iterator it = mDownloadHandles.rbegin(); it != mDownloadHandles.rend();)
    {
      rtFileDownloadHandle fileDownloadHandle = (*it);
      rtLogDebug("expires time: %f\n", fileDownloadHandle.expiresTime);
      if (fileDownloadHandle.origin == origin)
      {
        curlHandle = it->curlHandle;
        mDownloadHandles.erase(std::next(it).base());
        break;
      }
      else
      {
        ++it;
      }
    }
    if (curlHandle == NULL && !mDownloadHandles.empty())
    {
      curlHandle = mDownloadHandles.back().curlHandle;
      mDownloadHandles.pop_back();
    }
  }
  downloadHandleMutex.unlock();
#else
  curlHandle = curl_easy_init();
#endif //PX_REUSE_DOWNLOAD_HANDLES
  if (curlHandle == NULL)
  {
    curlHandle = curl_easy_init();
  }
  return curlHandle;
}

void rtFileDownloader::releaseDownloadHandle(CURL* curlHandle, double expiresTime, rtString& origin)
{
  rtLogDebug("expires time: %f", expiresTime);
#ifdef PX_REUSE_DOWNLOAD_HANDLES
    downloadHandleMutex.lock();
    static int numberOfDownloadHandles = rtThreadPool::globalInstance()->numberOfThreadsInPool();
    if(!mReuseDownloadHandles || mDownloadHandles.size() >= numberOfDownloadHandles || (expiresTime == 0))
    {
      curl_easy_cleanup(curlHandle);
    }
    else
    {
        if (expiresTime > 0)
        {
          expiresTime += pxSeconds();
        }
        mDownloadHandles.push_back(rtFileDownloadHandle(curlHandle, expiresTime, origin));
    }
    downloadHandleMutex.unlock();
#else
    curl_easy_cleanup(curlHandle);
#endif //PX_REUSE_DOWNLOAD_HANDLES
}

void rtFileDownloader::addFileDownloadRequest(rtFileDownloadRequest* downloadRequest)
{
  if (downloadRequest == NULL)
  {
    return;
  }
  mDownloadRequestVectorMutex->lock();
  bool found = false;
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    if ((*it) == downloadRequest)
    {
      found = true;
      break;
    }
  }
  if (!found)
  {
    mDownloadRequestVector->push_back(downloadRequest);
  }
  mDownloadRequestVectorMutex->unlock();
}

void rtFileDownloader::clearFileDownloadRequest(rtFileDownloadRequest* downloadRequest)
{
  mDownloadRequestVectorMutex->lock();
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    if ((*it) == downloadRequest)
    {
      mDownloadRequestVector->erase(it);
      break;
    }
  }
  if (downloadRequest != NULL)
  {
    delete downloadRequest;
  }
  mDownloadRequestVectorMutex->unlock();
}

void rtFileDownloader::setCallbackFunctionThreadSafe(rtFileDownloadRequest* downloadRequest,
                                                     void (*callbackFunction)(rtFileDownloadRequest*), void* owner)
{
  mDownloadRequestVectorMutex->lock();
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    if ((*it) == downloadRequest && (*it)->callbackData() == owner)
    {
      downloadRequest->setCallbackFunctionThreadSafe(callbackFunction);
      break;
    }
  }
  mDownloadRequestVectorMutex->unlock();
}

void rtFileDownloader::cancelAllDownloadRequestsThreadSafe()
{
  mDownloadRequestVectorMutex->lock();
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    (*it)->cancelRequest();
  }
  mDownloadRequestVectorMutex->unlock();
}

void rtFileDownloader::cancelDownloadRequestThreadSafe(rtFileDownloadRequest* downloadRequest, void* owner)
{
  mDownloadRequestVectorMutex->lock();
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    if ((*it) == downloadRequest && (*it)->callbackData() == owner)
    {
      downloadRequest->cancelRequest();
      break;
    }
  }
  mDownloadRequestVectorMutex->unlock();
}

bool rtFileDownloader::isDownloadRequestCanceled(rtFileDownloadRequest* downloadRequest, void* owner)
{
  bool requestIsCanceled = false;
  mDownloadRequestVectorMutex->lock();
  for (std::vector<rtFileDownloadRequest*>::iterator it=mDownloadRequestVector->begin(); it!=mDownloadRequestVector->end(); ++it)
  {
    if ((*it) == downloadRequest && (*it)->callbackData() == owner)
    {
      requestIsCanceled = downloadRequest->isCanceled();
      break;
    }
  }
  mDownloadRequestVectorMutex->unlock();
  return requestIsCanceled;
}

void rtFileDownloader::checkForExpiredHandles()
{
  rtLogDebug("inside checkForExpiredHandles");
  downloadHandleMutex.lock();
  for (vector<rtFileDownloadHandle>::iterator it = mDownloadHandles.begin(); it != mDownloadHandles.end();)
  {
    rtFileDownloadHandle fileDownloadHandle = (*it);
    rtLogDebug("expires time: %f\n", fileDownloadHandle.expiresTime);
    if (fileDownloadHandle.expiresTime < 0)
    {
      ++it;
      continue;
    }
    else if (pxSeconds() > fileDownloadHandle.expiresTime)
    {
      rtLogDebug("erasing handle!!!\n");
      curl_easy_cleanup(fileDownloadHandle.curlHandle);
      it = mDownloadHandles.erase(it);
    }
    else
    {
      ++it;
    }
  }
  downloadHandleMutex.unlock();
}
