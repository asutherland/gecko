/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset:  -*- */
/* vim:set expandtab ts=2 sw=2 sts=2 cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HttpLog.h"

#include "InterceptedChannel.h"
#include "nsInputStreamPump.h"
#include "nsIPipe.h"
#include "nsIStreamListener.h"
#include "nsHttpChannel.h"
#include "HttpChannelChild.h"
#include "nsHttpResponseHead.h"
#include "nsNetUtil.h"
#include "mozilla/ConsoleReportCollector.h"
#include "mozilla/dom/ChannelInfo.h"
#include "nsIChannelEventSink.h"

namespace mozilla {
namespace net {

extern bool
WillRedirect(const nsHttpResponseHead * response);

extern nsresult
DoUpdateExpirationTime(nsHttpChannel* aSelf,
                       nsICacheEntry* aCacheEntry,
                       nsHttpResponseHead* aResponseHead,
                       uint32_t& aExpirationTime);
extern nsresult
DoAddCacheEntryHeaders(nsHttpChannel *self,
                       nsICacheEntry *entry,
                       nsHttpRequestHead *requestHead,
                       nsHttpResponseHead *responseHead,
                       nsISupports *securityInfo);

NS_IMPL_ISUPPORTS(InterceptedChannelBase, nsIInterceptedChannel)

InterceptedChannelBase::InterceptedChannelBase(nsINetworkInterceptController* aController)
: mController(aController)
, mReportCollector(new ConsoleReportCollector())
, mClosed(false)
{
}

InterceptedChannelBase::~InterceptedChannelBase()
{
}

NS_IMETHODIMP
InterceptedChannelBase::GetResponseBody(nsIOutputStream** aStream)
{
  NS_IF_ADDREF(*aStream = mResponseBody);
  return NS_OK;
}

void
InterceptedChannelBase::EnsureSynthesizedResponse()
{
  if (mSynthesizedResponseHead.isNothing()) {
    mSynthesizedResponseHead.emplace(new nsHttpResponseHead());
  }
}

void
InterceptedChannelBase::DoNotifyController()
{
    nsresult rv = NS_OK;

    if (NS_WARN_IF(!mController)) {
      rv = ResetInterception();
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "Failed to resume intercepted network request");
      return;
    }

    rv = mController->ChannelIntercepted(this);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      rv = ResetInterception();
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "Failed to resume intercepted network request");
    }
    mController = nullptr;
}

nsresult
InterceptedChannelBase::DoSynthesizeStatus(uint16_t aStatus, const nsACString& aReason)
{
    EnsureSynthesizedResponse();

    // Always assume HTTP 1.1 for synthesized responses.
    nsAutoCString statusLine;
    statusLine.AppendLiteral("HTTP/1.1 ");
    statusLine.AppendInt(aStatus);
    statusLine.AppendLiteral(" ");
    statusLine.Append(aReason);

    (*mSynthesizedResponseHead)->ParseStatusLine(statusLine);
    return NS_OK;
}

nsresult
InterceptedChannelBase::DoSynthesizeHeader(const nsACString& aName, const nsACString& aValue)
{
    EnsureSynthesizedResponse();

    nsAutoCString header = aName + NS_LITERAL_CSTRING(": ") + aValue;
    // Overwrite any existing header.
    nsresult rv = (*mSynthesizedResponseHead)->ParseHeaderLine(header);
    NS_ENSURE_SUCCESS(rv, rv);
    return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelBase::GetConsoleReportCollector(nsIConsoleReportCollector** aCollectorOut)
{
  MOZ_ASSERT(aCollectorOut);
  nsCOMPtr<nsIConsoleReportCollector> ref = mReportCollector;
  ref.forget(aCollectorOut);
  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelBase::SetReleaseHandle(nsISupports* aHandle)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mReleaseHandle);
  MOZ_ASSERT(aHandle);

  // We need to keep it and mChannel alive until destructor clear it up.
  mReleaseHandle = aHandle;
  return NS_OK;
}

/* static */
already_AddRefed<nsIURI>
InterceptedChannelBase::SecureUpgradeChannelURI(nsIChannel* aChannel)
{
  nsCOMPtr<nsIURI> uri;
  nsresult rv = aChannel->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsCOMPtr<nsIURI> upgradedURI;
  rv = NS_GetSecureUpgradedURI(uri, getter_AddRefs(upgradedURI));
  NS_ENSURE_SUCCESS(rv, nullptr);

  return upgradedURI.forget();
}

InterceptedChannelChrome::InterceptedChannelChrome(nsHttpChannel* aChannel,
                                                   nsINetworkInterceptController* aController,
                                                   nsICacheEntry* aEntry)
: InterceptedChannelBase(aController)
, mChannel(aChannel)
, mSynthesizedCacheEntry(aEntry)
{
  nsresult rv = mChannel->GetApplyConversion(&mOldApplyConversion);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mOldApplyConversion = false;
  }
}

void
InterceptedChannelChrome::NotifyController()
{
  // Intercepted responses should already be decoded.
  mChannel->SetApplyConversion(false);

  nsresult rv = mSynthesizedCacheEntry->OpenOutputStream(0, getter_AddRefs(mResponseBody));
  NS_ENSURE_SUCCESS_VOID(rv);

  DoNotifyController();
}

NS_IMETHODIMP
InterceptedChannelChrome::GetChannel(nsIChannel** aChannel)
{
  NS_IF_ADDREF(*aChannel = mChannel);
  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelChrome::ResetInterception()
{
  if (mClosed) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  mReportCollector->FlushConsoleReports(mChannel);

  mSynthesizedCacheEntry->AsyncDoom(nullptr);
  mSynthesizedCacheEntry = nullptr;

  mChannel->SetApplyConversion(mOldApplyConversion);

  nsCOMPtr<nsIURI> uri;
  mChannel->GetURI(getter_AddRefs(uri));

  nsresult rv = mChannel->StartRedirectChannelToURI(uri, nsIChannelEventSink::REDIRECT_INTERNAL);
  NS_ENSURE_SUCCESS(rv, rv);

  mResponseBody->Close();
  mResponseBody = nullptr;
  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelChrome::SynthesizeStatus(uint16_t aStatus, const nsACString& aReason)
{
  if (!mSynthesizedCacheEntry) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return DoSynthesizeStatus(aStatus, aReason);
}

NS_IMETHODIMP
InterceptedChannelChrome::SynthesizeHeader(const nsACString& aName, const nsACString& aValue)
{
  if (!mSynthesizedCacheEntry) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return DoSynthesizeHeader(aName, aValue);
}

NS_IMETHODIMP
InterceptedChannelChrome::FinishSynthesizedResponse(const nsACString& aFinalURLSpec)
{
  if (mClosed) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Make sure the cache entry's output stream is always closed.  If the
  // channel was intercepted with a null-body response then its possible
  // the synthesis completed without a stream copy operation.
  mResponseBody->Close();

  mReportCollector->FlushConsoleReports(mChannel);

  EnsureSynthesizedResponse();

  // If the synthesized response is a redirect, then we want to respect
  // the encoding of whatever is loaded as a result.
  if (WillRedirect(mSynthesizedResponseHead.ref())) {
    nsresult rv = mChannel->SetApplyConversion(mOldApplyConversion);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  mChannel->MarkIntercepted();

  // First we ensure the appropriate metadata is set on the synthesized cache entry
  // (i.e. the flattened response head)

  nsCOMPtr<nsISupports> securityInfo;
  nsresult rv = mChannel->GetSecurityInfo(getter_AddRefs(securityInfo));
  NS_ENSURE_SUCCESS(rv, rv);

  uint32_t expirationTime = 0;
  rv = DoUpdateExpirationTime(mChannel, mSynthesizedCacheEntry,
                              mSynthesizedResponseHead.ref(),
                              expirationTime);

  rv = DoAddCacheEntryHeaders(mChannel, mSynthesizedCacheEntry,
                              mChannel->GetRequestHead(),
                              mSynthesizedResponseHead.ref(), securityInfo);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIURI> originalURI;
  mChannel->GetURI(getter_AddRefs(originalURI));

  nsCOMPtr<nsIURI> responseURI;
  if (!aFinalURLSpec.IsEmpty()) {
    rv = NS_NewURI(getter_AddRefs(responseURI), aFinalURLSpec);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    responseURI = originalURI;
  }

  bool equal = false;
  originalURI->Equals(responseURI, &equal);
  if (!equal) {
    rv =
        mChannel->StartRedirectChannelToURI(responseURI, nsIChannelEventSink::REDIRECT_INTERNAL);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    bool usingSSL = false;
    responseURI->SchemeIs("https", &usingSSL);

    // Then we open a real cache entry to read the synthesized response from.
    rv = mChannel->OpenCacheEntry(usingSSL);
    NS_ENSURE_SUCCESS(rv, rv);

    mSynthesizedCacheEntry = nullptr;

    if (!mChannel->AwaitingCacheCallbacks()) {
      rv = mChannel->ContinueConnect();
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelChrome::Cancel(nsresult aStatus)
{
  MOZ_ASSERT(NS_FAILED(aStatus));

  if (mClosed) {
    return NS_ERROR_FAILURE;
  }

  mReportCollector->FlushConsoleReports(mChannel);

  // we need to use AsyncAbort instead of Cancel since there's no active pump
  // to cancel which will provide OnStart/OnStopRequest to the channel.
  nsresult rv = mChannel->AsyncAbort(aStatus);
  NS_ENSURE_SUCCESS(rv, rv);

  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelChrome::SetChannelInfo(dom::ChannelInfo* aChannelInfo)
{
  if (mClosed) {
    return NS_ERROR_FAILURE;
  }

  return aChannelInfo->ResurrectInfoOnChannel(mChannel);
}

NS_IMETHODIMP
InterceptedChannelChrome::GetInternalContentPolicyType(nsContentPolicyType* aPolicyType)
{
  NS_ENSURE_ARG(aPolicyType);
  nsCOMPtr<nsILoadInfo> loadInfo;
  nsresult rv = mChannel->GetLoadInfo(getter_AddRefs(loadInfo));
  NS_ENSURE_SUCCESS(rv, rv);

  *aPolicyType = loadInfo->InternalContentPolicyType();
  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelChrome::GetSecureUpgradedChannelURI(nsIURI** aURI)
{
  return mChannel->GetURI(aURI);
}

/* [noscript,notxpcom] alreadyAddRefed_InternalRequest toInternalRequest (); */
NS_IMETHODIMP_(already_AddRefed<mozilla::dom::InternalRequest> *)
InterceptedChannelChrome::ToInternalRequest()
{
  AssertIsOnMainThread();
  nsCOMPtr<nsIChannel> channel;
  nsresult rv = GetChannel(getter_AddRefs(channel));
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsCOMPtr<nsIURI> uri;
  rv = GetSecureUpgradedChannelURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, nullptr);

  // Normally we rely on the Request constructor to strip the fragment, but
  // when creating the FetchEvent we bypass the constructor.  So strip the
  // fragment manually here instead.  We can't do it later when we create
  // the Request because that code executes off the main thread.
  nsCOMPtr<nsIURI> uriNoFragment;
  rv = uri->CloneIgnoringRef(getter_AddRefs(uriNoFragment));
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsCString spec;
  rv = uriNoFragment->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, nullptr);

  uint32_t loadFlags;
  rv = channel->GetLoadFlags(&loadFlags);
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsCOMPtr<nsILoadInfo> loadInfo;
  rv = channel->GetLoadInfo(getter_AddRefs(loadInfo));
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsContentPolicyType contentPolicyType = loadInfo->InternalContentPolicyType();

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(channel);
  MOZ_ASSERT(httpChannel, "How come we don't have an HTTP channel?");

  nsCString referrer;
  // Ignore the return value since the Referer header may not exist.
  httpChannel->GetRequestHeader(NS_LITERAL_CSTRING("Referer"), referrer);
  if (referrer.IsEmpty()) {
    referrer = NS_LITERAL_CSTRING("about:client");
  }

  uint32_t referrerPolicyCode = 0;
  rv = httpChannel->GetReferrerPolicy(&referrerPolicyCode);
  NS_ENSURE_SUCCESS(rv, nullptr);

  ReferrerPolicy referrerPolicy;
  switch (referrerPolicyCode) {
  case nsIHttpChannel::REFERRER_POLICY_NO_REFERRER:
    referrerPolicy = ReferrerPolicy::No_referrer;
    break;
  case nsIHttpChannel::REFERRER_POLICY_ORIGIN:
    referrerPolicy = ReferrerPolicy::Origin;
    break;
  case nsIHttpChannel::REFERRER_POLICY_NO_REFERRER_WHEN_DOWNGRADE:
    referrerPolicy = ReferrerPolicy::No_referrer_when_downgrade;
    break;
  case nsIHttpChannel::REFERRER_POLICY_ORIGIN_WHEN_XORIGIN:
    referrerPolicy = ReferrerPolicy::Origin_when_cross_origin;
    break;
  case nsIHttpChannel::REFERRER_POLICY_UNSAFE_URL:
    referrerPolicy = ReferrerPolicy::Unsafe_url;
    break;
  default:
    MOZ_ASSERT_UNREACHABLE("Invalid Referrer Policy enum value?");
    break;
  }

  nsCString method;
  rv = httpChannel->GetRequestMethod(method);
  NS_ENSURE_SUCCESS(rv, nullptr);

  nsCOMPtr<nsIHttpChannelInternal> internalChannel = do_QueryInterface(httpChannel);
  NS_ENSURE_TRUE(internalChannel, nullptr);

  RequestMode requestMode = InternalRequest::MapChannelToRequestMode(channel);

  // This is safe due to static_asserts in ServiceWorkerManager.cpp.
  uint32_t redirectModeCode;
  internalChannel->GetRedirectMode(&redirectModeCode);
  RequestRedirect requestRedirect = static_cast<RequestRedirect>(redirectModeCode);

  // This is safe due to static_asserts in ServiceWorkerManager.cpp.
  uint32_t cacheModeCode;
  internalChannel->GetFetchCacheMode(&cacheModeCode);
  RequestCache cacheMode = static_cast<RequestCache>(cacheModeCode);

  RequestCredentials requestCredentials = InternalRequest::MapChannelToRequestCredentials(channel);

  RefPtr<InternalHeaders> internalHeaders = new InternalHeaders();

  RefPtr<InternalRequest> ref = new InternalRequest(spec,
                                                    method,
                                                    internalHeaders.forget(),
                                                    cacheMode,
                                                    requestMode,
                                                    requestRedirect,
                                                    requestCredentials,
                                                    NS_ConvertUTF8toUTF16(referrer),
                                                    referrerPolicy,
                                                    contentPolicyType);

  return ref.forget();
}

/* [noscript,notxpcom] void synthesizeFromInternalResponse (in InternalResponsePtr aResponse); */
NS_IMETHODIMP_(void)
InterceptedChannelChrome::SynthesizeFromInternalResponse(mozilla::dom::InternalResponse *aResponse)
{
  nsresult rv;

  nsCOMPtr<nsIChannel> inner;
  rv = GetChannel(getter_AddRefs(inner));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }

  nsCOMPtr<nsILoadInfo> loadInfo = inner->GetLoadInfo();

  // TODO: CSP check

  RefPtr<InternalResponse> ir = aResponse;
  if (NS_WARN_IF(!ir)) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }

  MOZ_ASSERT(ir->GetChannelInfo().IsInitialized());
  rv = SetChannelInfo(const_cast<ChannelInfo*>(&ir->GetChannelInfo()));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }

  rv = SynthesizeStatus(ir->GetUnfilteredStatus(),
                        ir->GetUnfilteredStatusText());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }

  AutoTArray<InternalHeaders::Entry, 5> entries;
  ir->UnfilteredHeaders()->GetEntries(entries);
  for (uint32_t i = 0; i < entries.Length(); ++i) {
    SynthesizeHeader(entries[i].mName, entries[i].mValue);
  }

  loadInfo->MaybeIncreaseTainting(ir->GetTainting());

  nsCString responseURL;
  if (ir->Type() == ResponseType::Opaque) {
    ir->GetUnfilteredURL(responseURL);
    if (NS_WARN_IF(responseURL.IsEmpty())) {
      Cancel(NS_ERROR_INTERCEPTION_FAILED);
      return;
    }
  }

  // TODO: normally we block calling finish until copying completes...
  //       lets see if we really need to
  nsCOMPtr<nsIInputStream> bodyIn;
  ir->GetUnfilteredBody(getter_AddRefs(bodyIn));
  if (bodyIn) {
    nsCOMPtr<nsIOutputStream> bodyOut;
    rv = GetResponseBody(getter_AddRefs(bodyOut));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      Cancel(NS_ERROR_INTERCEPTION_FAILED);
      return;
    }

    const uint32_t kCopySegmentSize = 32 * 1024;

    if (!NS_OutputStreamIsBuffered(bodyOut)) {
      nsCOMPtr<nsIOutputStream> buffered;
      rv = NS_NewBufferedOutputStream(getter_AddRefs(buffered), bodyOut,
           kCopySegmentSize);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return;
      }
      bodyOut = buffered;
    }

    nsCOMPtr<nsIEventTarget> stsThread =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID, &rv);
    if (NS_WARN_IF(!stsThread)) {
      return;
    }

    rv = NS_AsyncCopy(bodyIn, bodyOut, stsThread, NS_ASYNCCOPY_VIA_WRITESEGMENTS,
                      kCopySegmentSize);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return;
    }
  }

  rv = FinishSynthesizedResponse(responseURL);
  if (NS_WARN_IF(!ir)) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }
}

InterceptedChannelContent::InterceptedChannelContent(HttpChannelChild* aChannel,
                                                     nsINetworkInterceptController* aController,
                                                     InterceptStreamListener* aListener,
                                                     bool aSecureUpgrade)
: InterceptedChannelBase(aController)
, mChannel(aChannel)
, mStreamListener(aListener)
, mSecureUpgrade(aSecureUpgrade)
{
}

void
InterceptedChannelContent::NotifyController()
{
  nsresult rv = NS_NewPipe(getter_AddRefs(mSynthesizedInput),
                           getter_AddRefs(mResponseBody),
                           0, UINT32_MAX, true, true);
  NS_ENSURE_SUCCESS_VOID(rv);

  DoNotifyController();
}

NS_IMETHODIMP
InterceptedChannelContent::GetChannel(nsIChannel** aChannel)
{
  NS_IF_ADDREF(*aChannel = mChannel);
  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelContent::ResetInterception()
{
  if (mClosed) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  mReportCollector->FlushConsoleReports(mChannel);

  mResponseBody->Close();
  mResponseBody = nullptr;
  mSynthesizedInput = nullptr;

  mChannel->ResetInterception();

  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelContent::SynthesizeStatus(uint16_t aStatus, const nsACString& aReason)
{
  if (!mResponseBody) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return DoSynthesizeStatus(aStatus, aReason);
}

NS_IMETHODIMP
InterceptedChannelContent::SynthesizeHeader(const nsACString& aName, const nsACString& aValue)
{
  if (!mResponseBody) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  return DoSynthesizeHeader(aName, aValue);
}

NS_IMETHODIMP
InterceptedChannelContent::FinishSynthesizedResponse(const nsACString& aFinalURLSpec)
{
  if (NS_WARN_IF(mClosed)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Make sure the body output stream is always closed.  If the channel was
  // intercepted with a null-body response then its possible the synthesis
  // completed without a stream copy operation.
  mResponseBody->Close();

  mReportCollector->FlushConsoleReports(mChannel);

  EnsureSynthesizedResponse();

  nsCOMPtr<nsIURI> originalURI;
  mChannel->GetURI(getter_AddRefs(originalURI));

  nsCOMPtr<nsIURI> responseURI;
  if (!aFinalURLSpec.IsEmpty()) {
    nsresult rv = NS_NewURI(getter_AddRefs(responseURI), aFinalURLSpec);
    NS_ENSURE_SUCCESS(rv, rv);
  } else if (mSecureUpgrade) {
    nsresult rv = NS_GetSecureUpgradedURI(originalURI,
                                          getter_AddRefs(responseURI));
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    responseURI = originalURI;
  }

  bool equal = false;
  originalURI->Equals(responseURI, &equal);
  if (!equal) {
    mChannel->ForceIntercepted(mSynthesizedInput);
    mChannel->BeginNonIPCRedirect(responseURI, *mSynthesizedResponseHead.ptr());
  } else {
    mChannel->OverrideWithSynthesizedResponse(mSynthesizedResponseHead.ref(),
                                              mSynthesizedInput,
                                              mStreamListener);
  }

  mResponseBody = nullptr;
  mStreamListener = nullptr;
  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelContent::Cancel(nsresult aStatus)
{
  MOZ_ASSERT(NS_FAILED(aStatus));

  if (mClosed) {
    return NS_ERROR_FAILURE;
  }

  mReportCollector->FlushConsoleReports(mChannel);

  // we need to use AsyncAbort instead of Cancel since there's no active pump
  // to cancel which will provide OnStart/OnStopRequest to the channel.
  nsresult rv = mChannel->AsyncAbort(aStatus);
  NS_ENSURE_SUCCESS(rv, rv);
  mStreamListener = nullptr;
  mClosed = true;

  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelContent::SetChannelInfo(dom::ChannelInfo* aChannelInfo)
{
  if (mClosed) {
    return NS_ERROR_FAILURE;
  }

  return aChannelInfo->ResurrectInfoOnChannel(mChannel);
}

NS_IMETHODIMP
InterceptedChannelContent::GetInternalContentPolicyType(nsContentPolicyType* aPolicyType)
{
  NS_ENSURE_ARG(aPolicyType);

  nsCOMPtr<nsILoadInfo> loadInfo;
  nsresult rv = mChannel->GetLoadInfo(getter_AddRefs(loadInfo));
  NS_ENSURE_SUCCESS(rv, rv);

  *aPolicyType = loadInfo->InternalContentPolicyType();
  return NS_OK;
}

NS_IMETHODIMP
InterceptedChannelContent::GetSecureUpgradedChannelURI(nsIURI** aURI)
{
  nsCOMPtr<nsIURI> uri;
  if (mSecureUpgrade) {
    uri = SecureUpgradeChannelURI(mChannel);
  } else {
    nsresult rv = mChannel->GetURI(getter_AddRefs(uri));
    NS_ENSURE_SUCCESS(rv, rv);
  }
  if (uri) {
    uri.forget(aURI);
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

} // namespace net
} // namespace mozilla
