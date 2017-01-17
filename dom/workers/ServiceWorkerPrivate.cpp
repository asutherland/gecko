/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerPrivate.h"

#include "ServiceWorkerManager.h"
#include "ServiceWorkerWindowClient.h"
#include "nsContentUtils.h"
#include "nsIHttpChannelInternal.h"
#include "nsIHttpHeaderVisitor.h"
#include "nsINetworkInterceptController.h"
#include "nsIPushErrorReporter.h"
#include "nsISupportsImpl.h"
#include "nsIUploadChannel2.h"
#include "nsFrameMessageManager.h" // for MessageManagerCallback; TODO remove.
#include "nsNetUtil.h"
#include "nsProxyRelease.h"
#include "nsQueryObject.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"
#include "ServiceWorkerInstanceParent.h"
#include "ServiceWorkerInstanceSpawner.h"
#include "WorkerRunnable.h"
#include "WorkerScope.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/FetchUtil.h"
#include "mozilla/dom/IndexedDatabaseManager.h"
#include "mozilla/dom/InternalHeaders.h"
#include "mozilla/dom/NotificationEvent.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/PushEventBinding.h"
#include "mozilla/dom/RequestBinding.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/Unused.h"

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

NS_IMPL_CYCLE_COLLECTING_ADDREF(ServiceWorkerPrivate)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ServiceWorkerPrivate)
NS_IMPL_CYCLE_COLLECTION(ServiceWorkerPrivate, mSupportsArray)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ServiceWorkerPrivate)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIObserver)
NS_INTERFACE_MAP_END

// Tracks the "dom.disable_open_click_delay" preference.  Modified on main
// thread, read on worker threads.
// It is updated every time a "notificationclick" event is dispatched. While
// this is done without synchronization, at the worst, the thread will just get
// an older value within which a popup is allowed to be displayed, which will
// still be a valid value since it was set prior to dispatching the runnable.
Atomic<uint32_t> gDOMDisableOpenClickDelay(0);

// Used to keep track of pending waitUntil as well as in-flight extendable events.
// When the last token is released, we attempt to terminate the worker.
class KeepAliveToken final : public nsISupports
{
public:
  NS_DECL_ISUPPORTS

  explicit KeepAliveToken(ServiceWorkerPrivate* aPrivate)
    : mPrivate(aPrivate)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aPrivate);
    mPrivate->AddToken();
  }

private:
  ~KeepAliveToken()
  {
    AssertIsOnMainThread();
    mPrivate->ReleaseToken();
  }

  RefPtr<ServiceWorkerPrivate> mPrivate;
};

NS_IMPL_ISUPPORTS0(KeepAliveToken)

ServiceWorkerPrivate::ServiceWorkerPrivate(ServiceWorkerInfo* aInfo)
  : mInfo(aInfo)
  , mDebuggerCount(0)
  , mTokenCount(0)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aInfo);

  mIdleWorkerTimer = do_CreateInstance(NS_TIMER_CONTRACTID);
  MOZ_ASSERT(mIdleWorkerTimer);
}

ServiceWorkerPrivate::~ServiceWorkerPrivate()
{
  MOZ_ASSERT(!mWorkerPrivate);
  MOZ_ASSERT(!mTokenCount);
  MOZ_ASSERT(!mInfo);
  MOZ_ASSERT(mSupportsArray.IsEmpty());

  mIdleWorkerTimer->Cancel();
}

nsresult
ServiceWorkerPrivate::SendMessageEvent(JSContext* aCx,
                                       JS::Handle<JS::Value> aMessage,
                                       const Optional<Sequence<JS::Value>>& aTransferable,
                                       UniquePtr<ServiceWorkerClientInfo>&& aClientInfo)
{
  // TESTING STOPGAP: As otherwise noted on our prototype declaration, the
  // correct API is for the ServiceWorker binding to handle the serialization.
  AssertIsOnMainThread();
  MOZ_ASSERT(XRE_IsParentProcess());

  ErrorResult rv(SpawnWorkerIfNeeded(MessageEvent));
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  // transferable boilerplate from WorkerPrivateParent::PostMessageInternal
  JS::Rooted<JS::Value> transferable(aCx, JS::UndefinedValue());
  if (aTransferable.WasPassed()) {
    const Sequence<JS::Value>& realTransferable = aTransferable.Value();

    // The input sequence only comes from the generated bindings code, which
    // ensures it is rooted.
    JS::HandleValueArray elements =
      JS::HandleValueArray::fromMarkedLocation(realTransferable.Length(),
                                               realTransferable.Elements());

    JSObject* array =
      JS_NewArrayObject(aCx, elements);
    if (!array) {
      rv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
    transferable.setObject(*array);
  }

  // perform the structured clone
  ipc::StructuredCloneData holder;
  holder->Write(aCx, aMessage, transferable, rv);
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  // serialize to IPC representation including transferables.
  ServiceWorkerEventArgs args(ServiceWorkerPostMessageEventArgs());
  ClonedMessageData& messageData =
    args.ServiceWorkerPostMessageEventArgs().messageData();

  if (!holder.BuildClonedMessageDataForParent(
        mServiceWorkerInstance->ContentParent(), messageData)) {
    return NS_ERROR_UNEXPECTED;
  }

  // TODO clients: The ClientInfo needs to be serialized.

  // (Not a functional event; we do not need to wait for "activated" or to
  // schedule an update check afterwards.)
  return SendEventCommon(args, nullptr);
}

nsresult
ServiceWorkerPrivate::SendEventCommon(ServiceWorkerEventArgs& args,
                                      LifeCycleEventCallback* aCallback)
{
  // Caller should have already called and result-checked SpawnWorkerIfNeeded.
  MOZ_ASSERT(mServiceWorkerInstance);

  PServiceWorkerEventParent* actor = new ServiceWorkerEventParent(
    this, args.type(), aCallback, nullptr);
  mServiceWorkerInstance->SendPServiceWorkerEventConstructor(actor, args);

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::SendFunctionalEvent(
  ServiceWorkerEventArgs& args,
  ServiceWorkerRegistrationInfo* aRegistration,
  nsIInterceptedChannel* aIntercepted)
{
  return SendEventCommon(args, nullptr, nullptr);
}

nsresult
ServiceWorkerPrivate::CheckScriptEvaluation(LifeCycleEventCallback* aCallback)
{
  MOZ_ASSERT(aCallback);

  nsresult rv = SpawnWorkerIfNeeded(LifeCycleEvent);
  if (NS_FAILED(rv)) {
    DispatchFalseLifeCycleEventCallback(aCallback);
    return rv;
  }

  ServiceWorkerEventArgs args(ServiceWorkerEvaluateScriptEventArgs());
  return SendEventCommon(args, aCallback);
}

nsresult
ServiceWorkerPrivate::SendLifeCycleEvent(const nsAString& aEventType,
                                         LifeCycleEventCallback* aCallback)
{
  nsresult rv = SpawnWorkerIfNeeded(LifeCycleEvent);
  if (NS_FAILED(rv)) {
    DispatchFalseLifeCycleEventCallback(aCallback);
    return rv;
  }

  ServiceWorkerLifeCycleEventArgs lifeCycleArgs(aEventType);
  ServiceWorkerEventArgs args(lifeCycleArgs);
  return SendEventCommon(args, aCallback);
}

namespace {

class PushErrorReporter final : public ExtendableEventCallback
{
  WorkerPrivate* mWorkerPrivate;
  nsString mMessageId;

  ~PushErrorReporter()
  {
  }

public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PushErrorReporter, override)

  PushErrorReporter(WorkerPrivate* aWorkerPrivate,
                    const nsAString& aMessageId)
    : mWorkerPrivate(aWorkerPrivate)
    , mMessageId(aMessageId)
  {
    mWorkerPrivate->AssertIsOnWorkerThread();
  }

  void
  FinishedWithResult(ExtendableEventResult aResult) override
  {
    if (aResult == Rejected) {
      Report(nsIPushErrorReporter::DELIVERY_UNHANDLED_REJECTION);
    }
  }

  void Report(uint16_t aReason = nsIPushErrorReporter::DELIVERY_INTERNAL_ERROR)
  {
    WorkerPrivate* workerPrivate = mWorkerPrivate;
    mWorkerPrivate->AssertIsOnWorkerThread();

    if (NS_WARN_IF(aReason > nsIPushErrorReporter::DELIVERY_INTERNAL_ERROR) ||
        mMessageId.IsEmpty()) {
      return;
    }
    nsCOMPtr<nsIRunnable> runnable =
      NewRunnableMethod<uint16_t>(this,
        &PushErrorReporter::ReportOnMainThread, aReason);
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(
      workerPrivate->DispatchToMainThread(runnable.forget())));
  }

  void ReportOnMainThread(uint16_t aReason)
  {
    AssertIsOnMainThread();
    nsCOMPtr<nsIPushErrorReporter> reporter =
      do_GetService("@mozilla.org/push/Service;1");
    if (reporter) {
      nsresult rv = reporter->ReportDeliveryError(mMessageId, aReason);
      Unused << NS_WARN_IF(NS_FAILED(rv));
    }
  }
};

} // anonymous namespace

nsresult
ServiceWorkerPrivate::SendPushEvent(const nsAString& aMessageId,
                                    const Maybe<nsTArray<uint8_t>>& aData,
                                    ServiceWorkerRegistrationInfo* aRegistration)
{
  nsresult rv = SpawnWorkerIfNeeded(PushEvent);
  if (NS_FAILED(rv)) {
    return rv;
  }

  MaybePushData maybePushData(aData ? *aData : void_t());
  ServiceWorkerPushEventArgs pushEventArgs(aMessageId, maybePushData);
  ServiceWorkerEventArgs args(pushEventArgs);
  return SendFunctionalEvent(args);

  nsresult rv = SpawnWorkerIfNeeded(, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<KeepAliveToken> token = CreateEventKeepAliveToken();

  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> regInfo(
    new nsMainThreadPtrHolder<ServiceWorkerRegistrationInfo>(aRegistration, false));

  RefPtr<WorkerRunnable> r = new SendPushEventRunnable(mWorkerPrivate,
                                                       token,
                                                       aMessageId,
                                                       aData,
                                                       regInfo);

  if (mInfo->State() == ServiceWorkerState::Activating) {
    mPendingFunctionalEvents.AppendElement(r.forget());
    return NS_OK;
  }

  MOZ_ASSERT(mInfo->State() == ServiceWorkerState::Activated);

  if (NS_WARN_IF(!r->Dispatch())) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::SendPushSubscriptionChangeEvent()
{
  nsresult rv = SpawnWorkerIfNeeded(PushSubscriptionChangeEvent, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<KeepAliveToken> token = CreateEventKeepAliveToken();
  RefPtr<WorkerRunnable> r =
    new SendPushSubscriptionChangeEventRunnable(mWorkerPrivate, token);
  if (NS_WARN_IF(!r->Dispatch())) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}


nsresult
ServiceWorkerPrivate::SendNotificationEvent(const nsAString& aEventName,
                                            const nsAString& aID,
                                            const nsAString& aTitle,
                                            const nsAString& aDir,
                                            const nsAString& aLang,
                                            const nsAString& aBody,
                                            const nsAString& aTag,
                                            const nsAString& aIcon,
                                            const nsAString& aData,
                                            const nsAString& aBehavior,
                                            const nsAString& aScope)
{
  WakeUpReason why;
  if (aEventName.EqualsLiteral(NOTIFICATION_CLICK_EVENT_NAME)) {
    why = NotificationClickEvent;
    gDOMDisableOpenClickDelay = Preferences::GetInt("dom.disable_open_click_delay");
  } else if (aEventName.EqualsLiteral(NOTIFICATION_CLOSE_EVENT_NAME)) {
    why = NotificationCloseEvent;
  } else {
    MOZ_ASSERT_UNREACHABLE("Invalid notification event name");
    return NS_ERROR_FAILURE;
  }

  nsresult rv = SpawnWorkerIfNeeded(why, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<KeepAliveToken> token = CreateEventKeepAliveToken();

  RefPtr<WorkerRunnable> r =
    new SendNotificationEventRunnable(mWorkerPrivate, token,
                                      aEventName, aID, aTitle, aDir, aLang,
                                      aBody, aTag, aIcon, aData, aBehavior,
                                      aScope);
  if (NS_WARN_IF(!r->Dispatch())) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::SendFetchEvent(nsIInterceptedChannel* aChannel,
                                     nsILoadGroup* aLoadGroup,
                                     const nsAString& aDocumentId,
                                     bool aIsReload)
{
  AssertIsOnMainThread();

  if (NS_WARN_IF(!mInfo)) {
    return NS_ERROR_FAILURE;
  }

  RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  MOZ_ASSERT(swm);

  RefPtr<ServiceWorkerRegistrationInfo> registration =
    swm->GetRegistration(mInfo->GetPrincipal(), mInfo->Scope());

  // Handle Fetch algorithm - step 16. If the service worker didn't register
  // any fetch event handlers, then abort the interception and maybe trigger
  // the soft update algorithm.
  if (!mInfo->HandlesFetch()) {
    aChannel->ResetInterception();

    // Trigger soft updates if necessary.
    registration->MaybeScheduleTimeCheckAndUpdate();

    return NS_OK;
  }

  // if the ServiceWorker script fails to load for some reason, just resume
  // the original channel.
  nsCOMPtr<nsIRunnable> failRunnable =
    NewRunnableMethod(aChannel, &nsIInterceptedChannel::ResetInterception);

  nsresult rv = SpawnWorkerIfNeeded(FetchEvent, failRunnable, aLoadGroup);
  NS_ENSURE_SUCCESS(rv, rv);

  nsMainThreadPtrHandle<nsIInterceptedChannel> handle(
    new nsMainThreadPtrHolder<nsIInterceptedChannel>(aChannel, false));

  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> regInfo(
    new nsMainThreadPtrHolder<ServiceWorkerRegistrationInfo>(registration, false));

  RefPtr<KeepAliveToken> token = CreateEventKeepAliveToken();

  RefPtr<FetchEventRunnable> r =
    new FetchEventRunnable(mWorkerPrivate, token, handle,
                           mInfo->ScriptSpec(), regInfo,
                           aDocumentId, aIsReload);
  rv = r->Init();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (mInfo->State() == ServiceWorkerState::Activating) {
    mPendingFunctionalEvents.AppendElement(r.forget());
    return NS_OK;
  }

  MOZ_ASSERT(mInfo->State() == ServiceWorkerState::Activated);

  if (NS_WARN_IF(!r->Dispatch())) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::SpawnWorkerIfNeeded(WakeUpReason aWhy)
{
  AssertIsOnMainThread();

  // XXXcatalinb: We need to have a separate load group that's linked to
  // an existing tab child to pass security checks on b2g.
  // This should be fixed in bug 1125961, but for now we enforce updating
  // the overriden load group when intercepting a fetch.
  MOZ_ASSERT_IF(aWhy == FetchEvent, aLoadGroup);

  if (mServiceWorkerInstance) {
    RenewIdleKeepAliveToken(aWhy);

    return NS_OK;
  }

  // Sanity check: mSupportsArray should be empty if we're about to
  // spin up a new worker.
  MOZ_ASSERT(mSupportsArray.IsEmpty());

  if (NS_WARN_IF(!mInfo)) {
    NS_WARNING("Trying to wake up a dead service worker.");
    return NS_ERROR_FAILURE;
  }

  mServiceWorkerInstance = ServiceWorkerInstanceSpawner::SpawnInstance(this);
  if (!mServiceWorkerInstance) {
    NS_WARNING("Failed to spawn remote ServiceWorker instance.");
    return NS_ERROR_FAILURE;
  }

  // TODO(catalinb): Bug 1192138 - Add telemetry for service worker wake-ups.

  RenewIdleKeepAliveToken(aWhy);

  return NS_OK;
}

void
ServiceWorkerPrivate::ServiceWorkerInstanceDestroyed(
  ServiceWorkerInstanceParent* aInstance)
{
  if (!mServiceWorkerInstance) {
    return;
  }

  MOZ_ASSERT(mServiceWorkerInstance == aInstance);
  mServiceWorkerInstance = nullptr;
}

void
ServiceWorkerPrivate::StoreISupports(nsISupports* aSupports)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(mWorkerPrivate);
  MOZ_ASSERT(!mSupportsArray.Contains(aSupports));

  mSupportsArray.AppendElement(aSupports);
}

void
ServiceWorkerPrivate::RemoveISupports(nsISupports* aSupports)
{
  AssertIsOnMainThread();
  mSupportsArray.RemoveElement(aSupports);
}

void
ServiceWorkerPrivate::TerminateWorker()
{
  AssertIsOnMainThread();

  mIdleWorkerTimer->Cancel();
  mIdleKeepAliveToken = nullptr;
  if (mServiceWorkerInstance) {
    if (Preferences::GetBool("dom.serviceWorkers.testing.enabled")) {
      nsCOMPtr<nsIObserverService> os = services::GetObserverService();
      if (os) {
        os->NotifyObservers(this, "service-worker-shutdown", nullptr);
      }
    }

    mServiceWorkerInstance->Terminate();
    mServiceWorkerInstance = nullptr;

    /// XXX mSupportsArray life-cycle needs determining
    mSupportsArray.Clear();

    // Any pending events are never going to fire on this worker.  Cancel
    // them so that intercepted channels can be reset and other resources
    // cleaned up.
    nsTArray<RefPtr<WorkerRunnable>> pendingEvents;
    mPendingFunctionalEvents.SwapElements(pendingEvents);
    for (uint32_t i = 0; i < pendingEvents.Length(); ++i) {
      pendingEvents[i]->Cancel();
    }
  }
}

void
ServiceWorkerPrivate::NoteDeadServiceWorkerInfo()
{
  AssertIsOnMainThread();
  mInfo = nullptr;
  TerminateWorker();
}

void
ServiceWorkerPrivate::Activated()
{
  AssertIsOnMainThread();

  // If we had to queue up events due to the worker activating, that means
  // the worker must be currently running.  We should be called synchronously
  // when the worker becomes activated.
  MOZ_ASSERT_IF(!mPendingFunctionalEvents.IsEmpty(), mWorkerPrivate);

  nsTArray<RefPtr<WorkerRunnable>> pendingEvents;
  mPendingFunctionalEvents.SwapElements(pendingEvents);

  for (uint32_t i = 0; i < pendingEvents.Length(); ++i) {
    RefPtr<WorkerRunnable> r = pendingEvents[i].forget();
    if (NS_WARN_IF(!r->Dispatch())) {
      NS_WARNING("Failed to dispatch pending functional event!");
    }
  }
}

nsresult
ServiceWorkerPrivate::GetDebugger(nsIWorkerDebugger** aResult)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aResult);

  if (!mDebuggerCount) {
    return NS_OK;
  }

  MOZ_ASSERT(mWorkerPrivate);

  nsCOMPtr<nsIWorkerDebugger> debugger = do_QueryInterface(mWorkerPrivate->Debugger());
  debugger.forget(aResult);

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::AttachDebugger()
{
  AssertIsOnMainThread();

  // When the first debugger attaches to a worker, we spawn a worker if needed,
  // and cancel the idle timeout. The idle timeout should not be reset until
  // the last debugger detached from the worker.
  if (!mDebuggerCount) {
    nsresult rv = SpawnWorkerIfNeeded(AttachEvent, nullptr);
    NS_ENSURE_SUCCESS(rv, rv);

    mIdleWorkerTimer->Cancel();
  }

  ++mDebuggerCount;

  return NS_OK;
}

nsresult
ServiceWorkerPrivate::DetachDebugger()
{
  AssertIsOnMainThread();

  if (!mDebuggerCount) {
    return NS_ERROR_UNEXPECTED;
  }

  --mDebuggerCount;

  // When the last debugger detaches from a worker, we either reset the idle
  // timeout, or terminate the worker if there are no more active tokens.
  if (!mDebuggerCount) {
    if (mTokenCount) {
      ResetIdleTimeout();
    } else {
      TerminateWorker();
    }
  }

  return NS_OK;
}

bool
ServiceWorkerPrivate::IsIdle() const
{
  AssertIsOnMainThread();
  return mTokenCount == 0 || (mTokenCount == 1 && mIdleKeepAliveToken);
}

/* static */ void
ServiceWorkerPrivate::NoteIdleWorkerCallback(nsITimer* aTimer, void* aPrivate)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aPrivate);

  RefPtr<ServiceWorkerPrivate> swp = static_cast<ServiceWorkerPrivate*>(aPrivate);

  MOZ_ASSERT(aTimer == swp->mIdleWorkerTimer, "Invalid timer!");

  // Release ServiceWorkerPrivate's token, since the grace period has ended.
  swp->mIdleKeepAliveToken = nullptr;

  if (swp->mWorkerPrivate) {
    // If we still have a workerPrivate at this point it means there are pending
    // waitUntil promises. Wait a bit more until we forcibly terminate the
    // worker.
    uint32_t timeout = Preferences::GetInt("dom.serviceWorkers.idle_extended_timeout");
    DebugOnly<nsresult> rv =
      swp->mIdleWorkerTimer->InitWithFuncCallback(ServiceWorkerPrivate::TerminateWorkerCallback,
                                                  aPrivate,
                                                  timeout,
                                                  nsITimer::TYPE_ONE_SHOT);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }
}

/* static */ void
ServiceWorkerPrivate::TerminateWorkerCallback(nsITimer* aTimer, void *aPrivate)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aPrivate);

  RefPtr<ServiceWorkerPrivate> serviceWorkerPrivate =
    static_cast<ServiceWorkerPrivate*>(aPrivate);

  MOZ_ASSERT(aTimer == serviceWorkerPrivate->mIdleWorkerTimer,
      "Invalid timer!");

  // mInfo must be non-null at this point because NoteDeadServiceWorkerInfo
  // which zeroes it calls TerminateWorker which cancels our timer which will
  // ensure we don't get invoked even if the nsTimerEvent is in the event queue.
  ServiceWorkerManager::LocalizeAndReportToAllClients(
    serviceWorkerPrivate->mInfo->Scope(),
    "ServiceWorkerGraceTimeoutTermination",
    nsTArray<nsString> { NS_ConvertUTF8toUTF16(serviceWorkerPrivate->mInfo->Scope()) });

  serviceWorkerPrivate->TerminateWorker();
}

void
ServiceWorkerPrivate::RenewIdleKeepAliveToken(WakeUpReason aWhy)
{
  // We should have an active worker if we're renewing the keep alive token.
  MOZ_ASSERT(mWorkerPrivate);

  // If there is at least one debugger attached to the worker, the idle worker
  // timeout was canceled when the first debugger attached to the worker. It
  // should not be reset until the last debugger detaches from the worker.
  if (!mDebuggerCount) {
    ResetIdleTimeout();
  }

  if (!mIdleKeepAliveToken) {
    mIdleKeepAliveToken = new KeepAliveToken(this);
  }
}

void
ServiceWorkerPrivate::ResetIdleTimeout()
{
  uint32_t timeout = Preferences::GetInt("dom.serviceWorkers.idle_timeout");
  DebugOnly<nsresult> rv =
    mIdleWorkerTimer->InitWithFuncCallback(ServiceWorkerPrivate::NoteIdleWorkerCallback,
                                           this, timeout,
                                           nsITimer::TYPE_ONE_SHOT);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

void
ServiceWorkerPrivate::AddToken()
{
  AssertIsOnMainThread();
  ++mTokenCount;
}

void
ServiceWorkerPrivate::ReleaseToken()
{
  AssertIsOnMainThread();

  MOZ_ASSERT(mTokenCount > 0);
  --mTokenCount;
  if (!mTokenCount) {
    TerminateWorker();
  } else if (IsIdle()) {
    RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
    if (swm) {
      swm->WorkerIsIdle(mInfo);
    }
  }
}

already_AddRefed<KeepAliveToken>
ServiceWorkerPrivate::CreateEventKeepAliveToken()
{
  AssertIsOnMainThread();
  MOZ_ASSERT(mWorkerPrivate);
  MOZ_ASSERT(mIdleKeepAliveToken);
  RefPtr<KeepAliveToken> ref = new KeepAliveToken(this);
  return ref.forget();
}

void
ServiceWorkerPrivate::FunctionalEventUpdateCheck()
{
  // If our worker is mooted, triggering an update check is no longer our
  // concern.
  if (!mInfo) {
    return;
  }

  RefPtr<ServiceWorkerRegistrationInfo> registration =
    swm->GetRegistration(mInfo->GetPrincipal(), mInfo->Scope());
  if (registration) {
    registration->MaybeScheduleTimeCheckAndUpdate();
  }
}

void
ServiceWorkerPrivate::AddPendingWindow(Runnable* aPendingWindow)
{
  AssertIsOnMainThread();
  pendingWindows.AppendElement(aPendingWindow);
}

nsresult
ServiceWorkerPrivate::Observe(nsISupports* aSubject, const char* aTopic, const char16_t* aData)
{
  AssertIsOnMainThread();

  nsCString topic(aTopic);
  if (!topic.Equals(NS_LITERAL_CSTRING("BrowserChrome:Ready"))) {
    MOZ_ASSERT(false, "Unexpected topic.");
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIObserverService> os = services::GetObserverService();
  NS_ENSURE_STATE(os);
  os->RemoveObserver(static_cast<nsIObserver*>(this), "BrowserChrome:Ready");

  size_t len = pendingWindows.Length();
  for (int i = len-1; i >= 0; i--) {
    RefPtr<Runnable> runnable = pendingWindows[i];
    MOZ_ALWAYS_SUCCEEDS(NS_DispatchToMainThread(runnable));
    pendingWindows.RemoveElementAt(i);
  }

  return NS_OK;
}

void
ServiceWorkerPrivate::SetHandlesFetch(bool aValue)
{
  AssertIsOnMainThread();

  if (NS_WARN_IF(!mInfo)) {
    return;
  }

  mInfo->SetHandlesFetch(aValue);
}

END_WORKERS_NAMESPACE
