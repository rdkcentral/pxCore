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

// rtFileDownloader.h

#ifndef RT_FILE_DOWNLOADER_H
#define RT_FILE_DOWNLOADER_H

#include "rtCore.h"
#include "rtString.h"
#ifdef ENABLE_HTTP_CACHE
#include <rtFileCache.h>
#endif
#include "rtCORS.h"

// TODO Eliminate std::string
#include <string.h>
#include <vector>

#if !defined(WIN32) && !defined(ENABLE_DFB)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#endif

#include <curl/curl.h>

#if !defined(WIN32) && !defined(ENABLE_DFB)
#pragma GCC diagnostic pop
#endif

class rtFileDownloadRequest
{
public:
   rtFileDownloadRequest(const char* imageUrl, void* callbackData, void (*callbackFunction)(rtFileDownloadRequest*) = NULL);
  ~rtFileDownloadRequest();

  void setTag(const char* name);
  rtString tag() const;

  void setFileUrl(const char* tag);
  rtString fileUrl() const;
  
  void setProxy(const char* proxyServer);
  rtString proxy() const;
  void setErrorString(const char* errorString);
  rtString errorString();
  void setCallbackFunction(void (*callbackFunction)(rtFileDownloadRequest*));
  void setExternalWriteCallback(size_t (*callbackFunction)(void *ptr, size_t size, size_t nmemb, void *userData), void *userPtr);
  void setProgressCallback(int (*callbackFunction)(void* ptr, double dltotal, double dlnow, double ultotal, double ulnow), void *userPtr);
  void* progressCallback(void);
  void* progressCallbackUserPtr(void);
  void setCallbackFunctionThreadSafe(void (*callbackFunction)(rtFileDownloadRequest*));
  long httpStatusCode();
  void setHttpStatusCode(long statusCode);
  bool executeCallback(int statusCode);
  size_t executeExternalWriteCallback(void *ptr, size_t size, size_t nmemb);
  void setDownloadedData(char* data, size_t size);
  void downloadedData(char*& data, size_t& size);
  char* downloadedData();
  size_t downloadedDataSize();
  void setHeaderData(char* data, size_t size);
  char* headerData();
  size_t headerDataSize();
  void setAdditionalHttpHeaders(std::vector<rtString>& additionalHeaders);
  std::vector<rtString>& additionalHttpHeaders();
  void setDownloadStatusCode(int statusCode);
  int downloadStatusCode();
  void* callbackData();
  void setCallbackData(void* callbackData);
  void setHeaderOnly(bool val);
  bool headerOnly();
  void setDownloadHandleExpiresTime(double timeInSeconds);
  double downloadHandleExpiresTime();
#ifdef ENABLE_HTTP_CACHE
  void setCacheEnabled(bool val);
  bool cacheEnabled();
  void setDataIsCached(bool val);
  size_t getCachedFileReadSize(void);
  void setCachedFileReadSize(size_t cachedFileReadSize);
  void setDeferCacheRead(bool val);
  bool deferCacheRead();
  FILE* cacheFilePointer(void);
#endif //ENABLE_HTTP_CACHE
  bool isDataCached();
  void setProgressMeter(bool val);
  bool isProgressMeterSwitchOff();
  void setUseCallbackDataSize(bool val);
  bool useCallbackDataSize();
  void setHTTPFailOnError(bool val);
  bool isHTTPFailOnError();
  void setHTTPError(const char* httpError);
  char* httpErrorBuffer(void);
  void setCurlDefaultTimeout(bool val);
  bool isCurlDefaultTimeoutSet();
  void setConnectionTimeout(long val);
  long getConnectionTimeout();
  void setCORS(const rtCORSRef& cors);
  rtCORSRef cors() const;
  void cancelRequest();
  bool isCanceled();
  void setMethod(const char* method);
  rtString method() const;
  void setReadData(const uint8_t* data, size_t size);
  const uint8_t* readData() const;
  size_t readDataSize() const;
  void enableDownloadMetrics(bool enableDownloadMetrics);
  bool isDownloadMetricsEnabled(void);
  rtObjectRef downloadMetrics() const;
  void setDownloadMetrics(int32_t connectTimeMs, int32_t sslConnectTimeMs, int32_t totalTimeMs, int32_t downloadSpeedBytesPerSecond);
  void setDownloadOnly(bool downloadOnly);
  bool isDownloadOnly(void);
  void setActualFileSize(size_t actualFileSize);
  size_t actualFileSize(void);
  void setByteRangeEnable(bool bByteRangeFlag);
  bool isByteRangeEnabled(void);
  void setByteRangeIntervals(size_t byteRangeIntervals);
  size_t byteRangeIntervals(void);
  void setCurlErrRetryCount(unsigned int curlRetryCount);
  unsigned int getCurlErrRetryCount(void);
  void setCurlRetryEnable(bool bCurlRetry);
  bool isCurlRetryEnabled(void);
  void setUseEncoding(bool useEncoding);
  bool isUseEncoding() const;
  void setUserAgent(const char* userAgent);
  rtString userAgent() const;
  void setRedirectFollowLocation(bool redirectFollowLocation);
  bool isRedirectFollowLocationEnabled(void);
  void setKeepTCPAlive(bool keepTCPAlive);
  bool keepTCPAliveEnabled(void);
  void setCROSRequired(bool crosRequired);
  bool isCROSRequired(void);
  void setVerifySSL(bool verifySSL);
  bool isVerifySSLEnabled(void);

private:
  rtString mTag;
  rtString mFileUrl;
  rtString mProxyServer;
  rtString mErrorString;
  long mHttpStatusCode;
  void (*mCallbackFunction)(rtFileDownloadRequest*);
  size_t (*mExternalWriteCallback)(void *ptr, size_t size, size_t nmemb, void *userData);
  void *mExternalWriteCallbackUserPtr;
  int (*mProgressCallback)(void* ptr, double TotalToDownload, double NowDownloaded, double TotalToUpload, double NowUploaded);
  void *mProgressCallbackUserPtr;
  char* mDownloadedData;
  size_t mDownloadedDataSize;
  int mDownloadStatusCode;
  void* mCallbackData;
  rtMutex mCallbackFunctionMutex;
  char* mHeaderData;
  size_t mHeaderDataSize;
  std::vector<rtString> mAdditionalHttpHeaders;
  bool mHeaderOnly;
  double mDownloadHandleExpiresTime;
#ifdef ENABLE_HTTP_CACHE
  bool mCacheEnabled;
  bool mDeferCacheRead;
  size_t mCachedFileReadSize;
#endif //ENABLE_HTTP_CACHE
  bool mIsDataInCache;
  bool mIsProgressMeterSwitchOff;
  bool mHTTPFailOnError;
  char mHttpErrorBuffer[CURL_ERROR_SIZE];
  bool mDefaultTimeout;
  long mConnectionTimeout;
  rtCORSRef mCORS;
  bool mCanceled;
  bool mUseCallbackDataSize;
  rtMutex mCanceledMutex;
  rtString mMethod;
  const uint8_t* mReadData;
  size_t mReadDataSize;
  bool mDownloadMetricsEnabled;
  rtObjectRef mDownloadMetrics;
  bool mDownloadOnly;
  size_t mActualFileSize;
  bool mIsByteRangeEnabled;
  size_t mByteRangeIntervals;
  unsigned int curlErrRetryCount;
  bool mCurlRetry;
  bool mUseEncoding;
  rtString mUserAgent;
  bool mRedirectFollowLocation;
  bool mKeepTCPAlive;
  bool mCROSRequired;
};

struct rtFileDownloadHandle
{
  rtFileDownloadHandle(CURL* handle) : curlHandle(handle), expiresTime(-1), origin() {}
  rtFileDownloadHandle(CURL* handle, double time, rtString hostOrigin) : curlHandle(handle), expiresTime(time), origin(hostOrigin) {}
  CURL* curlHandle;
  double expiresTime;
  rtString origin;
};

class rtFileDownloader
{
public:

    static rtFileDownloader* instance();
    static void deleteInstance();
    static void setCallbackFunctionThreadSafe(rtFileDownloadRequest* downloadRequest, void (*callbackFunction)(rtFileDownloadRequest*), void* owner);
    static void cancelAllDownloadRequestsThreadSafe();
    static void cancelDownloadRequestThreadSafe(rtFileDownloadRequest* downloadRequest, void* owner);
    static bool isDownloadRequestCanceled(rtFileDownloadRequest* downloadRequest, void* owner);

    virtual bool addToDownloadQueue(rtFileDownloadRequest* downloadRequest);
    virtual void raiseDownloadPriority(rtFileDownloadRequest* downloadRequest);
    virtual void removeDownloadRequest(rtFileDownloadRequest* downloadRequest);

    void clearFileCache();
    void downloadFile(rtFileDownloadRequest* downloadRequest);
    void downloadFileAsByteRange(rtFileDownloadRequest* downloadRequest);
    void setDefaultCallbackFunction(void (*callbackFunction)(rtFileDownloadRequest*));
    bool downloadFromNetwork(rtFileDownloadRequest* downloadRequest);
    bool downloadByteRangeFromNetwork(rtFileDownloadRequest* downloadRequest, bool *bRedirect);
    void checkForExpiredHandles();

private:
    rtFileDownloader();
    ~rtFileDownloader();

    void startNextDownload(rtFileDownloadRequest* downloadRequest);
    rtFileDownloadRequest* nextDownloadRequest();
    void startNextDownloadInBackground();
    void downloadFileInBackground(rtFileDownloadRequest* downloadRequest);
#ifdef ENABLE_HTTP_CACHE
    bool checkAndDownloadFromCache(rtFileDownloadRequest* downloadRequest,rtHttpCacheData& cachedData);
#endif
    CURL* retrieveDownloadHandle(rtString& origin);
    void releaseDownloadHandle(CURL* curlHandle, double expiresTime, rtString& origin );
    static void addFileDownloadRequest(rtFileDownloadRequest* downloadRequest);
    static void clearFileDownloadRequest(rtFileDownloadRequest* downloadRequest);
    //todo: hash mPendingDownloadRequests;
    //todo: string list mPendingDownloadOrderList;
    //todo: list mActiveDownloads;
    unsigned int mNumberOfCurrentDownloads;
    //todo: hash m_priorityDownloads;
    void (*mDefaultCallbackFunction)(rtFileDownloadRequest*);
    std::vector<rtFileDownloadHandle> mDownloadHandles;
    bool mReuseDownloadHandles;
    rtString mCaCertFile;
    rtMutex mFileCacheMutex;
    static rtFileDownloader* mInstance;
    static std::vector<rtFileDownloadRequest*>* mDownloadRequestVector;
    static rtMutex* mDownloadRequestVectorMutex;
};

#endif //RT_FILE_DOWNLOADER_H
