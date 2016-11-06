/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

NS_IMPL_ISUPPORTS0(ServiceWorkerEventChild)

ServiceWorkerEventChild::ServiceWorkerEventChild(
  ServiceWorkerInstanceChild *aOwner)
  : mOwner(aOwner)
{
    MOZ_COUNT_CTOR(ServiceWorkerEventChild);
}

void
ServiceWorkerEventChild::Init(const ServiceWorkerEventArgs& aArgs)
{
  switch (aArgs.type()) {
    case ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs:
      StartEvaluateScript(aArgs.ServiceWorkerEvaluateScriptEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerLifeCycleEventArgs:
      StartLifeCycle(aArgs.ServiceWorkerLifeCycleEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs:
      StartFetchEvent(aArgs.ServiceWorkerFetchEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPostMessageEventArgs:
      StartPostMessage(aArgs.ServiceWorkerPostMessageEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPushEventArgs:
      StartPush(aArgs.ServiceWorkerPushEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPushSubscriptionChangeEventArgs:
      StartPushSubscriptionChange(aArgs.ServiceWorkerPushSubscriptionChangeEventArgs());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerNotificationEventArgs:
      StartNotification(aArgs.ServiceWorkerNotificationEventArgs());
      break;
  }
}

ServiceWorkerEventChild::~ServiceWorkerEventChild()
{
    MOZ_COUNT_DTOR(ServiceWorkerEventChild);
}

namespace {

class CheckScriptEvaluationRunnable final : public WorkerRunnable
{
  RefPtr<ServiceWorkerEventChild> mEvent;

public:
  CheckScriptEvaluationRunnable(WorkerPrivate* aWorkerPrivate,
                                ServiceWorkerEventChild* aEvent)
    : WorkerRunnable(aWorkerPrivate)
    , mEvent(aEvent)
#ifdef DEBUG
    , mDone(false)
#endif
  {
    AssertIsOnMainThread();
  }

  ~CheckScriptEvaluationWithCallback()
  {
    MOZ_ASSERT(mDone);
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    aWorkerPrivate->AssertIsOnWorkerThread();

    bool fetchHandlerWasAdded = aWorkerPrivate->FetchHandlerWasAdded();
    nsCOMPtr<nsIRunnable> runnable = NewRunnableMethod<bool>(this,
      &CheckScriptEvaluationWithCallback::ReportFetchFlag, fetchHandlerWasAdded);
    aWorkerPrivate->DispatchToMainThread(runnable.forget());

    ReportScriptEvaluationResult(aWorkerPrivate->WorkerScriptExecutedSuccessfully());

    return true;
  }

  void
  ReportFetchFlag(bool aFetchHandlerWasAdded)
  {
    AssertIsOnMainThread();
    mServiceWorkerPrivate->SetHandlesFetch(aFetchHandlerWasAdded);
  }

  nsresult
  Cancel() override
  {
    ReportScriptEvaluationResult(false);
    return WorkerRunnable::Cancel();
  }

private:
  void
  ReportScriptEvaluationResult(bool aScriptEvaluationResult)
  {
#ifdef DEBUG
    mDone = true;
#endif
    MOZ_ALWAYS_SUCCEEDS(mWorkerPrivate->DispatchToMainThread(
      NewRunnableMethod(mEvent, &ServiceWorkerEventChild::DoneEvaluateScript,
                        aResult)));
  }
};

} // anonymous namespace

void
ServiceWorkerEventChild::StartEvaluateScript(
  const ServiceWorkerEvaluateScriptEventArgs &aArgs)
{
  RefPtr<WorkerRunnable> r = new CheckScriptEvaluationWithCallback(
    mOwner->WorkerPrivate(), mOwner);

  if (NS_WARN_IF(!r->Dispatch())) {
    DoneEvaluateScript(false);
  }
}

void
ServiceWorkerEventChild::DoneEvaluateScript(bool aResult)
{
  ServiceWorkerEventResult result(
    ServiceWorkerEvaluateScriptEventResult(aResult));
  Send__delete__(this, result);
}

namespace {

enum ExtendableEventResult {
    Rejected = 0,
    Resolved
};

class ExtendableEventCallback {
public:
  virtual void
  FinishedWithResult(ExtendableEventResult aResult) = 0;

  NS_IMETHOD_(MozExternalRefCountType)
  AddRef() = 0;

  NS_IMETHOD_(MozExternalRefCountType)
  Release() = 0;
};

class KeepAliveHandler final : public WorkerHolder
                             , public ExtendableEvent::ExtensionsHandler
                             , public PromiseNativeHandler
{
  // This class manages lifetime extensions added by calling WaitUntil()
  // or RespondWith(). We allow new extensions as long as we still hold
  // |mKeepAliveToken|. Once the last promise was settled, we queue a microtask
  // which releases the token and prevents further extensions. By doing this,
  // we give other pending microtasks a chance to continue adding extensions.

  nsMainThreadPtrHandle<KeepAliveToken> mKeepAliveToken;
  WorkerPrivate* MOZ_NON_OWNING_REF mWorkerPrivate;
  bool mWorkerHolderAdded;

  // We start holding a self reference when the first extension promise is
  // added. As far as I can tell, the only case where this is useful is when
  // we're waiting indefinitely on a promise that's no longer reachable
  // and will never be settled.
  // The cycle is broken when the last promise was settled or when the
  // worker is shutting down.
  RefPtr<KeepAliveHandler> mSelfRef;

  // Called when the last promise was settled.
  RefPtr<ExtendableEventCallback> mCallback;

  uint32_t mPendingPromisesCount;

  // We don't actually care what values the promises resolve to, only whether
  // any of them were rejected.
  bool mRejected;

public:
  NS_DECL_ISUPPORTS

  explicit KeepAliveHandler(const nsMainThreadPtrHandle<KeepAliveToken>& aKeepAliveToken,
                            ExtendableEventCallback* aCallback)
    : mKeepAliveToken(aKeepAliveToken)
    , mWorkerPrivate(GetCurrentThreadWorkerPrivate())
    , mWorkerHolderAdded(false)
    , mCallback(aCallback)
    , mPendingPromisesCount(0)
    , mRejected(false)
  {
    MOZ_ASSERT(mKeepAliveToken);
    MOZ_ASSERT(mWorkerPrivate);
  }

  bool
  UseWorkerHolder()
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(!mWorkerHolderAdded);
    mWorkerHolderAdded = HoldWorker(mWorkerPrivate, Terminating);
    return mWorkerHolderAdded;
  }

  bool
  WaitOnPromise(Promise& aPromise) override
  {
    if (!mKeepAliveToken) {
      MOZ_ASSERT(!mSelfRef, "We shouldn't be holding a self reference!");
      return false;
    }
    if (!mSelfRef) {
      MOZ_ASSERT(!mPendingPromisesCount);
      mSelfRef = this;
    }

    ++mPendingPromisesCount;
    aPromise.AppendNativeHandler(this);

    return true;
  }

  void
  ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override
  {
    RemovePromise(Resolved);
  }

  void
  RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override
  {
    RemovePromise(Rejected);
  }

  bool
  Notify(Status aStatus) override
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    if (aStatus < Terminating) {
      return true;
    }

    MaybeCleanup();
    return true;
  }

  void
  MaybeDone()
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();

    if (mPendingPromisesCount) {
      return;
    }
    if (mCallback) {
      mCallback->FinishedWithResult(mRejected ? Rejected : Resolved);
    }

    MaybeCleanup();
  }

private:
  ~KeepAliveHandler()
  {
    MaybeCleanup();
  }

  void
  MaybeCleanup()
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    if (!mKeepAliveToken) {
      return;
    }
    if (mWorkerHolderAdded) {
      ReleaseWorker();
    }

    mKeepAliveToken = nullptr;
    mSelfRef = nullptr;
  }

  void
  RemovePromise(ExtendableEventResult aResult)
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_DIAGNOSTIC_ASSERT(mPendingPromisesCount > 0);
    MOZ_ASSERT(mSelfRef);
    MOZ_ASSERT(mKeepAliveToken);

    mRejected |= (aResult == Rejected);

    --mPendingPromisesCount;
    if (mPendingPromisesCount) {
      return;
    }

    CycleCollectedJSContext* cx = CycleCollectedJSContext::Get();
    MOZ_ASSERT(cx);

    RefPtr<nsIRunnable> r = NewRunnableMethod(this, &KeepAliveHandler::MaybeDone);
    cx->DispatchToMicroTask(r.forget());
  }

NS_IMPL_ISUPPORTS0(KeepAliveHandler)

class ExtendableEventWorkerRunnable : public WorkerRunnable
{
protected:
  nsMainThreadPtrHandle<KeepAliveToken> mKeepAliveToken;

public:
  ExtendableEventWorkerRunnable(WorkerPrivate* aWorkerPrivate,
                                KeepAliveToken* aKeepAliveToken)
    : WorkerRunnable(aWorkerPrivate)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aWorkerPrivate);
    MOZ_ASSERT(aKeepAliveToken);

    mKeepAliveToken =
      new nsMainThreadPtrHolder<KeepAliveToken>(aKeepAliveToken);
  }

  nsresult
  DispatchExtendableEventOnWorkerScope(JSContext* aCx,
                                       WorkerGlobalScope* aWorkerScope,
                                       ExtendableEvent* aEvent,
                                       ExtendableEventCallback* aCallback)
  {
    MOZ_ASSERT(aWorkerScope);
    MOZ_ASSERT(aEvent);
    nsCOMPtr<nsIGlobalObject> sgo = aWorkerScope;
    WidgetEvent* internalEvent = aEvent->WidgetEventPtr();

    RefPtr<KeepAliveHandler> keepAliveHandler =
      new KeepAliveHandler(mKeepAliveToken, aCallback);
    if (NS_WARN_IF(!keepAliveHandler->UseWorkerHolder())) {
      return NS_ERROR_FAILURE;
    }

    // This must always be set *before* dispatching the event, otherwise
    // waitUntil calls will fail.
    aEvent->SetKeepAliveHandler(keepAliveHandler);

    ErrorResult result;
    result = aWorkerScope->DispatchDOMEvent(nullptr, aEvent, nullptr, nullptr);
    if (NS_WARN_IF(result.Failed())) {
      result.SuppressException();
      return NS_ERROR_FAILURE;
    }

    // [[ If e’s extend lifetime promises is empty, unset e’s extensions allowed
    //    flag and abort these steps. ]]
    keepAliveHandler->MaybeDone();

    // We don't block the event when getting an exception but still report the
    // error message.
    // Report exception message. Note: This will not stop the event.
    if (internalEvent->mFlags.mExceptionWasRaised) {
      result.SuppressException();
      return NS_ERROR_XPC_JS_THREW_EXCEPTION;
    }

    return NS_OK;
  }
};

class SendMesssageEventRunnable final : public ExtendableEventWorkerRunnable
                                      , public StructuredCloneHolder
{
  UniquePtr<ServiceWorkerClientInfo> mEventSource;

public:
  SendMesssageEventRunnable(WorkerPrivate*  aWorkerPrivate,
                            KeepAliveToken* aKeepAliveToken,
                            UniquePtr<ServiceWorkerClientInfo>&& aEventSource)
    : ExtendableEventWorkerRunnable(aWorkerPrivate, aKeepAliveToken)
    , StructuredCloneHolder(CloningSupported, TransferringSupported,
                            StructuredCloneScope::SameProcessDifferentThread)
    , mEventSource(Move(aEventSource))
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(mEventSource);
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    JS::Rooted<JS::Value> messageData(aCx);
    nsCOMPtr<nsIGlobalObject> sgo = aWorkerPrivate->GlobalScope();
    ErrorResult rv;
    Read(sgo, aCx, &messageData, rv);
    if (NS_WARN_IF(rv.Failed())) {
      return true;
    }

    Sequence<OwningNonNull<MessagePort>> ports;
    if (!TakeTransferredPortsAsSequence(ports)) {
      return true;
    }

    RefPtr<ServiceWorkerClient> client = new ServiceWorkerWindowClient(sgo,
                                                                       *mEventSource);
    RootedDictionary<ExtendableMessageEventInit> init(aCx);

    init.mBubbles = false;
    init.mCancelable = false;

    init.mData = messageData;
    init.mPorts = ports;
    init.mSource.SetValue().SetAsClient() = client;

    RefPtr<EventTarget> target = aWorkerPrivate->GlobalScope();
    RefPtr<ExtendableMessageEvent> extendableEvent =
      ExtendableMessageEvent::Constructor(target, NS_LITERAL_STRING("message"),
                                          init, rv);
    if (NS_WARN_IF(rv.Failed())) {
      rv.SuppressException();
      return false;
    }

    extendableEvent->SetTrusted(true);

    return NS_SUCCEEDED(DispatchExtendableEventOnWorkerScope(aCx,
                                                             aWorkerPrivate->GlobalScope(),
                                                             extendableEvent,
                                                             nullptr));
  }
};

} // anonymous namespace

nsresult
ServiceWorkerPrivate::SendMessageEvent(JSContext* aCx,
                                       JS::Handle<JS::Value> aMessage,
                                       const Optional<Sequence<JS::Value>>& aTransferable,
                                       UniquePtr<ServiceWorkerClientInfo>&& aClientInfo)
{
  AssertIsOnMainThread();

  ErrorResult rv(SpawnWorkerIfNeeded(MessageEvent, nullptr));
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  JS::Rooted<JS::Value> transferable(aCx, JS::UndefinedHandleValue);
  if (aTransferable.WasPassed()) {
    const Sequence<JS::Value>& value = aTransferable.Value();
    JS::HandleValueArray elements =
      JS::HandleValueArray::fromMarkedLocation(value.Length(), value.Elements());

    JSObject* array = JS_NewArrayObject(aCx, elements);
    if (!array) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
    transferable.setObject(*array);
  }
  RefPtr<KeepAliveToken> token = CreateEventKeepAliveToken();
  RefPtr<SendMesssageEventRunnable> runnable =
    new SendMesssageEventRunnable(mWorkerPrivate, token, Move(aClientInfo));

  runnable->Write(aCx, aMessage, transferable, JS::CloneDataPolicy(), rv);
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  if (!runnable->Dispatch()) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

namespace {

// Handle functional event
// 9.9.7 If the time difference in seconds calculated by the current time minus
// registration's last update check time is greater than 86400, invoke Soft Update
// algorithm.
class ExtendableFunctionalEventWorkerRunnable : public ExtendableEventWorkerRunnable
{
protected:
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mRegistration;
public:
  ExtendableFunctionalEventWorkerRunnable(WorkerPrivate* aWorkerPrivate,
                                          KeepAliveToken* aKeepAliveToken,
                                          nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo>& aRegistration)
    : ExtendableEventWorkerRunnable(aWorkerPrivate, aKeepAliveToken)
    , mRegistration(aRegistration)
  {
    MOZ_ASSERT(aRegistration);
  }

  void
  PostRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate, bool aRunResult)
  {
    nsCOMPtr<nsIRunnable> runnable =
      new RegistrationUpdateRunnable(mRegistration, true /* time check */);
    aWorkerPrivate->DispatchToMainThread(runnable.forget());

    ExtendableEventWorkerRunnable::PostRun(aCx, aWorkerPrivate, aRunResult);
  }
};

NS_IMPL_ISUPPORTS0(ExtendableEventWorkerRunnable::InternalHandler)

/*
 * Fires 'install' event on the ServiceWorkerGlobalScope. Modifies busy count
 * since it fires the event. This is ok since there can't be nested
 * ServiceWorkers, so the parent thread -> worker thread requirement for
 * runnables is satisfied.
 */
class LifecycleEventWorkerRunnable : public ExtendableEventWorkerRunnable
{
  RefPtr<ServiceWorkerEventChild> mEvent;
  nsString mEventName;

public:
  LifecycleEventWorkerRunnable(WorkerPrivate* aWorkerPrivate,
                               ServiceWorkerEventChild* aEvent,
                               const nsAString& aEventName)
      : ExtendableEventWorkerRunnable(aWorkerPrivate)
      , mEvent(aEvent)
      , mEventName(aEventName)
  {
    AssertIsOnMainThread();
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);
    return DispatchLifecycleEvent(aCx, aWorkerPrivate);
  }

  nsresult
  Cancel() override
  {
    Done(false);
    return WorkerRunnable::Cancel();
  }

private:
  bool
  DispatchLifecycleEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate);

};

/*
 * Used to handle ExtendableEvent::waitUntil() and catch abnormal worker
 * termination during the execution of life cycle events. It is responsible
 * with advancing the job queue for install/activate tasks.
 */
class LifeCycleEventWatcher final : public ExtendableEventCallback,
                                    public WorkerHolder
{
  WorkerPrivate* mWorkerPrivate;
  RefPtr<LifeCycleEventCallback> mCallback;
  bool mDone;

  ~LifeCycleEventWatcher()
  {
    if (mDone) {
      return;
    }

    MOZ_ASSERT(GetCurrentThreadWorkerPrivate() == mWorkerPrivate);
    // XXXcatalinb: If all the promises passed to waitUntil go out of scope,
    // the resulting Promise.all will be cycle collected and it will drop its
    // native handlers (including this object). Instead of waiting for a timeout
    // we report the failure now.
    ReportResult(false);
  }

public:
  NS_INLINE_DECL_REFCOUNTING(LifeCycleEventWatcher, override)

  LifeCycleEventWatcher(WorkerPrivate* aWorkerPrivate,
                        LifeCycleEventCallback* aCallback)
    : mWorkerPrivate(aWorkerPrivate)
    , mCallback(aCallback)
    , mDone(false)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
  }

  bool
  Init()
  {
    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();

    // We need to listen for worker termination in case the event handler
    // never completes or never resolves the waitUntil promise. There are
    // two possible scenarios:
    // 1. The keepAlive token expires and the worker is terminated, in which
    //    case the registration/update promise will be rejected
    // 2. A new service worker is registered which will terminate the current
    //    installing worker.
    if (NS_WARN_IF(!HoldWorker(mWorkerPrivate, Terminating))) {
      NS_WARNING("LifeCycleEventWatcher failed to add feature.");
      ReportResult(false);
      return false;
    }

    return true;
  }

  bool
  Notify(Status aStatus) override
  {
    if (aStatus < Terminating) {
      return true;
    }

    MOZ_ASSERT(GetCurrentThreadWorkerPrivate() == mWorkerPrivate);
    ReportResult(false);

    return true;
  }

  void
  ReportResult(bool aResult)
  {
    mWorkerPrivate->AssertIsOnWorkerThread();

    if (mDone) {
      return;
    }
    mDone = true;

    mCallback->SetResult(aResult);
    nsresult rv = mWorkerPrivate->DispatchToMainThread(mCallback);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      MOZ_CRASH("Failed to dispatch life cycle event handler.");
    }

    ReleaseWorker();
  }

  void
  FinishedWithResult(ExtendableEventResult aResult) override
  {
    MOZ_ASSERT(GetCurrentThreadWorkerPrivate() == mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    ReportResult(aResult == Resolved);

    // Note, all WaitUntil() rejections are reported to client consoles
    // by the WaitUntilHandler in ServiceWorkerEvents.  This ensures that
    // errors in non-lifecycle events like FetchEvent and PushEvent are
    // reported properly.
  }
};

void
ServiceWorkerEventChild::StartLifeCycle(const ServiceWorkerLifeCycleEventArgs &aArgs)
{
  RefPtr<WorkerRunnable> r = new LifecycleEventWorkerRunnable(
    mOwner->WorkerPrivate(), mOwner);

  if (NS_WARN_IF(!r->Dispatch())) {
    DoneLifeCycle(false);
  }
}

bool
LifecycleEventWorkerRunnable::DispatchLifecycleEvent(JSContext* aCx,
                                                     WorkerPrivate* aWorkerPrivate)
{
  aWorkerPrivate->AssertIsOnWorkerThread();
  MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());

  RefPtr<ExtendableEvent> event;
  RefPtr<EventTarget> target = aWorkerPrivate->GlobalScope();

  if (mEventName.EqualsASCII("install") || mEventName.EqualsASCII("activate")) {
    ExtendableEventInit init;
    init.mBubbles = false;
    init.mCancelable = false;
    event = ExtendableEvent::Constructor(target, mEventName, init);
  } else {
    MOZ_CRASH("Unexpected lifecycle event");
  }

  event->SetTrusted(true);

  // It is important to initialize the watcher before actually dispatching
  // the event in order to catch worker termination while the event handler
  // is still executing. This can happen with infinite loops, for example.
  RefPtr<LifeCycleEventWatcher> watcher =
    new LifeCycleEventWatcher(aWorkerPrivate, mCallback);

  if (!watcher->Init()) {
    return true;
  }

  nsresult rv = DispatchExtendableEventOnWorkerScope(aCx,
                                                     aWorkerPrivate->GlobalScope(),
                                                     event,
                                                     watcher);
  // Do not fail event processing when an exception is thrown.
  if (NS_FAILED(rv) && rv != NS_ERROR_XPC_JS_THREW_EXCEPTION) {
    watcher->ReportResult(false);
  }

  return true;
}

} // anonymous namespace

void
ServiceWorkerEventChild::StartLifeCycle(const ServiceWorkerLifeCycleEventArgs &aArgs)
{
  RefPtr<WorkerRunnable> r = new LifecycleEventWorkerRunnable(
    mOwner->WorkerPrivate(), mOwner, aArgs.eventName());

  if (NS_WARN_IF(!r->Dispatch())) {
    DoneLifeCycle(false);
  }
}

void
ServiceWorkerEventChild::DoneLifeCycle(bool aResult)
{
  ServiceWorkerEventResult result(
    ServiceWorkerLifeCycleEventResult(aResult));
  Send__delete__(this, result);
}

namespace {

/*

 */
class PostMessageEventWorkerRunnable : public ExtendableEventWorkerRunnable
{
  RefPtr<ServiceWorkerEventChild> mEvent;
  ipc::StructuredCloneData mCloneData;

public:
  // Extract/transfer ownership of the serialize clone data from aMessageData
  // during construction (because the IPC layer will destroy it once we return
  // control flow), but do not create any actors (ex: PBlob) until we get to the
  // background thread.
  PostMessageEventWorkerRunnable(WorkerPrivate* aWorkerPrivate,
                                 ServiceWorkerEventChild* aEvent,
                                 ClonedMessageData &aMessageData)
      : ExtendableEventWorkerRunnable(aWorkerPrivate)
      , mEvent(aEvent)
  {
    AssertIsOnMainThread();

    // Transfer the serialized buffer's ownership.
    aData.UseExternalData(aMessageData.data().data);

    // XXX Okay, so the RemoteBlobImpl is totally fine with being accessed
    // from different threads.  It's very smart.  Hooray.  So we are able to
    // do the unpacking on this thread and do the Read() on the worker.
    ipc::UnpackClonedMessageDataForChild(aMessageData, mCloneData);

  }

  // Now we
  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);



    return DispatchLifecycleEvent(aCx, aWorkerPrivate);
  }

  nsresult
  Cancel() override
  {
    Done(false);
    return WorkerRunnable::Cancel();
  }

  void
  Done(bool aSuccess) override
  {
    MOZ_ALWAYS_SUCCEEDS(mWorkerPrivate->DispatchToMainThread(
      NewRunnableMethod(mEvent, &ServiceWorkerEventChild::DonePostMessage)));
  }

private:
  bool
  DispatchLifecycleEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate);

};

} // anonymous namespace

void
ServiceWorkerEventChild::StartPostMessage(const ServiceWorkerPostMessageEventArgs& aArgs)
{
  RefPtr<WorkerRunnable> r = new PostMessageEventWorkerRunnable(
    mOwner->WorkerPrivate(), mOwner, aArgs.messageData());

  if (NS_WARN_IF(!r->Dispatch())) {
    DonePostMessage();
  }
  mOwner->WorkerPrivate()->PostMessageToServiceWorker
}

void
ServiceWorkerEventChild::DonePostMessage()
{

}

class SendPushEventRunnable final : public ExtendableEventWorkerRunnable
{
  RefPtr<ServiceWorkerEventChild> mEvent;
  nsString mMessageId;
  Maybe<nsTArray<uint8_t>> mData;

public:
  SendPushEventRunnable(WorkerPrivate* aWorkerPrivate,
                        ServiceWorkerEventChild* aEvent,
                        const nsAString& aMessageId,
                        const Maybe<nsTArray<uint8_t>>& aData)
      : ExtendableEventWorkerRunnable(aWorkerPrivate)
      , mEvent(aEvent)
      , mMessageId(aMessageId)
      , mData(aData)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aWorkerPrivate);
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);
    GlobalObject globalObj(aCx, aWorkerPrivate->GlobalScope()->GetWrapper());

    RefPtr<PushErrorReporter> errorReporter =
      new PushErrorReporter(aWorkerPrivate, mMessageId);

    PushEventInit pei;
    if (mData) {
      const nsTArray<uint8_t>& bytes = mData.ref();
      JSObject* data = Uint8Array::Create(aCx, bytes.Length(), bytes.Elements());
      if (!data) {
        DoneWithError();
        errorReporter->Report();
        return false;
      }
      pei.mData.Construct().SetAsArrayBufferView().Init(data);
    }
    pei.mBubbles = false;
    pei.mCancelable = false;

    ErrorResult result;
    RefPtr<PushEvent> event =
      PushEvent::Constructor(globalObj, NS_LITERAL_STRING("push"), pei, result);
    if (NS_WARN_IF(result.Failed())) {
      result.SuppressException();
      errorReporter->Report();
      return false;
    }
    event->SetTrusted(true);

    nsresult rv = DispatchExtendableEventOnWorkerScope(aCx,
                                                       aWorkerPrivate->GlobalScope(),
                                                       event,
                                                       errorReporter);
    if (NS_FAILED(rv)) {
      // We don't cancel WorkerPrivate when catching an excetpion.
      errorReporter->Report(nsIPushErrorReporter::DELIVERY_UNCAUGHT_EXCEPTION);
    }

    return true;
  }

  void
  Done(bool aSuccess)
  {
    Done();
  }

  void
  DoneWithErrorCode(uint16_t aReason)
  {

  }
};

class SendPushSubscriptionChangeEventRunnable final : public ExtendableEventWorkerRunnable
{

public:
  explicit SendPushSubscriptionChangeEventRunnable(
    WorkerPrivate* aWorkerPrivate, KeepAliveToken* aKeepAliveToken)
      : ExtendableEventWorkerRunnable(aWorkerPrivate, aKeepAliveToken)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aWorkerPrivate);
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);

    RefPtr<EventTarget> target = aWorkerPrivate->GlobalScope();

    ExtendableEventInit init;
    init.mBubbles = false;
    init.mCancelable = false;

    RefPtr<ExtendableEvent> event =
      ExtendableEvent::Constructor(target,
                                   NS_LITERAL_STRING("pushsubscriptionchange"),
                                   init);

    event->SetTrusted(true);

    DispatchExtendableEventOnWorkerScope(aCx, aWorkerPrivate->GlobalScope(),
                                         event, nullptr);

    return true;
  }

  void
  Done(bool aSuccess)
  {

  }
};

static void
DummyNotificationTimerCallback(nsITimer* aTimer, void* aClosure)
{
  // Nothing.
}

class AllowWindowInteractionHandler;

class ClearWindowAllowedRunnable final : public WorkerRunnable
{
public:
  ClearWindowAllowedRunnable(WorkerPrivate* aWorkerPrivate,
                             AllowWindowInteractionHandler* aHandler)
  : WorkerRunnable(aWorkerPrivate, WorkerThreadUnchangedBusyCount)
  , mHandler(aHandler)
  { }

private:
  bool
  PreDispatch(WorkerPrivate* aWorkerPrivate) override
  {
    // WorkerRunnable asserts that the dispatch is from parent thread if
    // the busy count modification is WorkerThreadUnchangedBusyCount.
    // Since this runnable will be dispatched from the timer thread, we override
    // PreDispatch and PostDispatch to skip the check.
    return true;
  }

  void
  PostDispatch(WorkerPrivate* aWorkerPrivate, bool aDispatchResult) override
  {
    // Silence bad assertions.
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override;

  nsresult
  Cancel() override
  {
    // Always ensure the handler is released on the worker thread, even if we
    // are cancelled.
    mHandler = nullptr;
    return WorkerRunnable::Cancel();
  }

  RefPtr<AllowWindowInteractionHandler> mHandler;
};

class AllowWindowInteractionHandler final : public ExtendableEventCallback
{
  friend class ClearWindowAllowedRunnable;
  nsCOMPtr<nsITimer> mTimer;

  ~AllowWindowInteractionHandler()
  {
  }

  void
  ClearWindowAllowed(WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();

    if (!mTimer) {
      return;
    }

    // XXXcatalinb: This *might* be executed after the global was unrooted, in
    // which case GlobalScope() will return null. Making the check here just
    // to be safe.
    WorkerGlobalScope* globalScope = aWorkerPrivate->GlobalScope();
    if (!globalScope) {
      return;
    }

    globalScope->ConsumeWindowInteraction();
    mTimer->Cancel();
    mTimer = nullptr;
    MOZ_ALWAYS_TRUE(aWorkerPrivate->ModifyBusyCountFromWorker(false));
  }

  void
  StartClearWindowTimer(WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(!mTimer);

    nsresult rv;
    nsCOMPtr<nsITimer> timer = do_CreateInstance(NS_TIMER_CONTRACTID, &rv);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return;
    }

    RefPtr<ClearWindowAllowedRunnable> r =
      new ClearWindowAllowedRunnable(aWorkerPrivate, this);

    RefPtr<TimerThreadEventTarget> target =
      new TimerThreadEventTarget(aWorkerPrivate, r);

    rv = timer->SetTarget(target);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return;
    }

    // The important stuff that *has* to be reversed.
    if (NS_WARN_IF(!aWorkerPrivate->ModifyBusyCountFromWorker(true))) {
      return;
    }
    aWorkerPrivate->GlobalScope()->AllowWindowInteraction();
    timer.swap(mTimer);

    // We swap first and then initialize the timer so that even if initializing
    // fails, we still clean the busy count and interaction count correctly.
    // The timer can't be initialized before modifying the busy count since the
    // timer thread could run and call the timeout but the worker may
    // already be terminating and modifying the busy count could fail.
    rv = mTimer->InitWithFuncCallback(DummyNotificationTimerCallback, nullptr,
                                      gDOMDisableOpenClickDelay,
                                      nsITimer::TYPE_ONE_SHOT);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      ClearWindowAllowed(aWorkerPrivate);
      return;
    }
  }

public:
  NS_INLINE_DECL_REFCOUNTING(AllowWindowInteractionHandler, override)

  explicit AllowWindowInteractionHandler(WorkerPrivate* aWorkerPrivate)
  {
    StartClearWindowTimer(aWorkerPrivate);
  }

  void
  FinishedWithResult(ExtendableEventResult /* aResult */) override
  {
    WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
    ClearWindowAllowed(workerPrivate);
  }
};

bool
ClearWindowAllowedRunnable::WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
{
  mHandler->ClearWindowAllowed(aWorkerPrivate);
  mHandler = nullptr;
  return true;
}

class SendNotificationEventRunnable final : public ExtendableEventWorkerRunnable
{
  const nsString mEventName;
  const nsString mID;
  const nsString mTitle;
  const nsString mDir;
  const nsString mLang;
  const nsString mBody;
  const nsString mTag;
  const nsString mIcon;
  const nsString mData;
  const nsString mBehavior;
  const nsString mScope;

public:
  SendNotificationEventRunnable(WorkerPrivate* aWorkerPrivate,
                                KeepAliveToken* aKeepAliveToken,
                                const nsAString& aEventName,
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
      : ExtendableEventWorkerRunnable(aWorkerPrivate, aKeepAliveToken)
      , mEventName(aEventName)
      , mID(aID)
      , mTitle(aTitle)
      , mDir(aDir)
      , mLang(aLang)
      , mBody(aBody)
      , mTag(aTag)
      , mIcon(aIcon)
      , mData(aData)
      , mBehavior(aBehavior)
      , mScope(aScope)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aWorkerPrivate);
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);

    RefPtr<EventTarget> target = do_QueryObject(aWorkerPrivate->GlobalScope());

    ErrorResult result;
    RefPtr<Notification> notification =
      Notification::ConstructFromFields(aWorkerPrivate->GlobalScope(), mID,
                                        mTitle, mDir, mLang, mBody, mTag, mIcon,
                                        mData, mScope, result);
    if (NS_WARN_IF(result.Failed())) {
      return false;
    }

    NotificationEventInit nei;
    nei.mNotification = notification;
    nei.mBubbles = false;
    nei.mCancelable = false;

    RefPtr<NotificationEvent> event =
      NotificationEvent::Constructor(target, mEventName,
                                     nei, result);
    if (NS_WARN_IF(result.Failed())) {
      return false;
    }

    event->SetTrusted(true);
    aWorkerPrivate->GlobalScope()->AllowWindowInteraction();
    RefPtr<AllowWindowInteractionHandler> allowWindowInteraction =
      new AllowWindowInteractionHandler(aWorkerPrivate);
    nsresult rv = DispatchExtendableEventOnWorkerScope(aCx,
                                                       aWorkerPrivate->GlobalScope(),
                                                       event,
                                                       allowWindowInteraction);
    // Don't reject when catching an exception
    if (NS_FAILED(rv) && rv != NS_ERROR_XPC_JS_THREW_EXCEPTION) {
      allowWindowInteraction->FinishedWithResult(Rejected);
    }
    aWorkerPrivate->GlobalScope()->ConsumeWindowInteraction();

    return true;
  }
};

// Inheriting ExtendableEventWorkerRunnable so that the worker is not terminated
// while handling the fetch event, though that's very unlikely.
class FetchEventRunnable : public ExtendableFunctionalEventWorkerRunnable
                         , public nsIHttpHeaderVisitor {
  nsMainThreadPtrHandle<nsIInterceptedChannel> mInterceptedChannel;
  const nsCString mScriptSpec;
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mRegistration;
  nsTArray<nsCString> mHeaderNames;
  nsTArray<nsCString> mHeaderValues;
  nsCString mSpec;
  nsCString mFragment;
  nsCString mMethod;
  nsString mClientId;
  bool mIsReload;
  RequestCache mCacheMode;
  RequestMode mRequestMode;
  RequestRedirect mRequestRedirect;
  RequestCredentials mRequestCredentials;
  nsContentPolicyType mContentPolicyType;
  nsCOMPtr<nsIInputStream> mUploadStream;
  nsCString mReferrer;
  ReferrerPolicy mReferrerPolicy;
  nsString mIntegrity;
public:
  FetchEventRunnable(WorkerPrivate* aWorkerPrivate,
                     KeepAliveToken* aKeepAliveToken,
                     nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel,
                     // CSP checks might require the worker script spec
                     // later on.
                     const nsACString& aScriptSpec,
                     nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo>& aRegistration,
                     const nsAString& aDocumentId,
                     bool aIsReload)
    : ExtendableFunctionalEventWorkerRunnable(
        aWorkerPrivate, aKeepAliveToken, aRegistration)
    , mInterceptedChannel(aChannel)
    , mScriptSpec(aScriptSpec)
    , mRegistration(aRegistration)
    , mClientId(aDocumentId)
    , mIsReload(aIsReload)
    , mCacheMode(RequestCache::Default)
    , mRequestMode(RequestMode::No_cors)
    , mRequestRedirect(RequestRedirect::Follow)
    // By default we set it to same-origin since normal HTTP fetches always
    // send credentials to same-origin websites unless explicitly forbidden.
    , mRequestCredentials(RequestCredentials::Same_origin)
    , mContentPolicyType(nsIContentPolicy::TYPE_INVALID)
    , mReferrer(kFETCH_CLIENT_REFERRER_STR)
    , mReferrerPolicy(ReferrerPolicy::_empty)
  {
    MOZ_ASSERT(aWorkerPrivate);
  }

  NS_DECL_ISUPPORTS_INHERITED

  NS_IMETHOD
  VisitHeader(const nsACString& aHeader, const nsACString& aValue) override
  {
    mHeaderNames.AppendElement(aHeader);
    mHeaderValues.AppendElement(aValue);
    return NS_OK;
  }

  nsresult
  Init()
  {
    AssertIsOnMainThread();
    nsCOMPtr<nsIChannel> channel;
    nsresult rv = mInterceptedChannel->GetChannel(getter_AddRefs(channel));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIURI> uri;
    rv = mInterceptedChannel->GetSecureUpgradedChannelURI(getter_AddRefs(uri));
    NS_ENSURE_SUCCESS(rv, rv);

    // Normally we rely on the Request constructor to strip the fragment, but
    // when creating the FetchEvent we bypass the constructor.  So strip the
    // fragment manually here instead.  We can't do it later when we create
    // the Request because that code executes off the main thread.
    nsCOMPtr<nsIURI> uriNoFragment;
    rv = uri->CloneIgnoringRef(getter_AddRefs(uriNoFragment));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = uriNoFragment->GetSpec(mSpec);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = uri->GetRef(mFragment);
    NS_ENSURE_SUCCESS(rv, rv);

    uint32_t loadFlags;
    rv = channel->GetLoadFlags(&loadFlags);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsILoadInfo> loadInfo;
    rv = channel->GetLoadInfo(getter_AddRefs(loadInfo));
    NS_ENSURE_SUCCESS(rv, rv);

    mContentPolicyType = loadInfo->InternalContentPolicyType();

    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(channel);
    MOZ_ASSERT(httpChannel, "How come we don't have an HTTP channel?");

    nsAutoCString referrer;
    // Ignore the return value since the Referer header may not exist.
    httpChannel->GetRequestHeader(NS_LITERAL_CSTRING("Referer"), referrer);
    if (!referrer.IsEmpty()) {
      mReferrer = referrer;
    } else {
      // If there's no referrer Header, means the header was omitted for
      // security/privacy reason.
      mReferrer = EmptyCString();
    }

    uint32_t referrerPolicy = 0;
    rv = httpChannel->GetReferrerPolicy(&referrerPolicy);
    NS_ENSURE_SUCCESS(rv, rv);
    switch (referrerPolicy) {
      case nsIHttpChannel::REFERRER_POLICY_UNSET:
      mReferrerPolicy = ReferrerPolicy::_empty;
      break;
    case nsIHttpChannel::REFERRER_POLICY_NO_REFERRER:
      mReferrerPolicy = ReferrerPolicy::No_referrer;
      break;
    case nsIHttpChannel::REFERRER_POLICY_ORIGIN:
      mReferrerPolicy = ReferrerPolicy::Origin;
      break;
    case nsIHttpChannel::REFERRER_POLICY_NO_REFERRER_WHEN_DOWNGRADE:
      mReferrerPolicy = ReferrerPolicy::No_referrer_when_downgrade;
      break;
    case nsIHttpChannel::REFERRER_POLICY_ORIGIN_WHEN_XORIGIN:
      mReferrerPolicy = ReferrerPolicy::Origin_when_cross_origin;
      break;
    case nsIHttpChannel::REFERRER_POLICY_UNSAFE_URL:
      mReferrerPolicy = ReferrerPolicy::Unsafe_url;
      break;
    case nsIHttpChannel::REFERRER_POLICY_SAME_ORIGIN:
      mReferrerPolicy = ReferrerPolicy::Same_origin;
      break;
    case nsIHttpChannel::REFERRER_POLICY_STRICT_ORIGIN_WHEN_XORIGIN:
      mReferrerPolicy = ReferrerPolicy::Strict_origin_when_cross_origin;
      break;
    case nsIHttpChannel::REFERRER_POLICY_STRICT_ORIGIN:
      mReferrerPolicy = ReferrerPolicy::Strict_origin;
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Invalid Referrer Policy enum value?");
      break;
    }

    rv = httpChannel->GetRequestMethod(mMethod);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIHttpChannelInternal> internalChannel = do_QueryInterface(httpChannel);
    NS_ENSURE_TRUE(internalChannel, NS_ERROR_NOT_AVAILABLE);

    mRequestMode = InternalRequest::MapChannelToRequestMode(channel);

    // This is safe due to static_asserts in ServiceWorkerManager.cpp.
    uint32_t redirectMode;
    internalChannel->GetRedirectMode(&redirectMode);
    mRequestRedirect = static_cast<RequestRedirect>(redirectMode);

    // This is safe due to static_asserts in ServiceWorkerManager.cpp.
    uint32_t cacheMode;
    internalChannel->GetFetchCacheMode(&cacheMode);
    mCacheMode = static_cast<RequestCache>(cacheMode);

    internalChannel->GetIntegrityMetadata(mIntegrity);

    mRequestCredentials = InternalRequest::MapChannelToRequestCredentials(channel);

    rv = httpChannel->VisitNonDefaultRequestHeaders(this);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIUploadChannel2> uploadChannel = do_QueryInterface(httpChannel);
    if (uploadChannel) {
      MOZ_ASSERT(!mUploadStream);
      bool bodyHasHeaders = false;
      rv = uploadChannel->GetUploadStreamHasHeaders(&bodyHasHeaders);
      NS_ENSURE_SUCCESS(rv, rv);
      nsCOMPtr<nsIInputStream> uploadStream;
      rv = uploadChannel->CloneUploadStream(getter_AddRefs(uploadStream));
      NS_ENSURE_SUCCESS(rv, rv);
      if (bodyHasHeaders) {
        HandleBodyWithHeaders(uploadStream);
      } else {
        mUploadStream = uploadStream;
      }
    }

    return NS_OK;
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate) override
  {
    MOZ_ASSERT(aWorkerPrivate);
    return DispatchFetchEvent(aCx, aWorkerPrivate);
  }

  nsresult
  Cancel() override
  {
    nsCOMPtr<nsIRunnable> runnable = new ResumeRequest(mInterceptedChannel);
    if (NS_FAILED(mWorkerPrivate->DispatchToMainThread(runnable))) {
      NS_WARNING("Failed to resume channel on FetchEventRunnable::Cancel()!\n");
    }
    WorkerRunnable::Cancel();
    return NS_OK;
  }

private:
  ~FetchEventRunnable() {}

  class ResumeRequest final : public Runnable {
    nsMainThreadPtrHandle<nsIInterceptedChannel> mChannel;
  public:
    explicit ResumeRequest(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel)
      : mChannel(aChannel)
    {
    }

    NS_IMETHOD Run() override
    {
      AssertIsOnMainThread();
      nsresult rv = mChannel->ResetInterception();
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "Failed to resume intercepted network request");
      return rv;
    }
  };

  bool
  DispatchFetchEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aCx);
    MOZ_ASSERT(aWorkerPrivate);
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
    GlobalObject globalObj(aCx, aWorkerPrivate->GlobalScope()->GetWrapper());

    RefPtr<InternalHeaders> internalHeaders = new InternalHeaders(HeadersGuardEnum::Request);
    MOZ_ASSERT(mHeaderNames.Length() == mHeaderValues.Length());
    for (uint32_t i = 0; i < mHeaderNames.Length(); i++) {
      ErrorResult result;
      internalHeaders->Set(mHeaderNames[i], mHeaderValues[i], result);
      if (NS_WARN_IF(result.Failed())) {
        result.SuppressException();
        return false;
      }
    }

    ErrorResult result;
    internalHeaders->SetGuard(HeadersGuardEnum::Immutable, result);
    if (NS_WARN_IF(result.Failed())) {
      result.SuppressException();
      return false;
    }
    RefPtr<InternalRequest> internalReq = new InternalRequest(mSpec,
                                                              mFragment,
                                                              mMethod,
                                                              internalHeaders.forget(),
                                                              mCacheMode,
                                                              mRequestMode,
                                                              mRequestRedirect,
                                                              mRequestCredentials,
                                                              NS_ConvertUTF8toUTF16(mReferrer),
                                                              mReferrerPolicy,
                                                              mContentPolicyType,
                                                              mIntegrity);
    internalReq->SetBody(mUploadStream);
    // For Telemetry, note that this Request object was created by a Fetch event.
    internalReq->SetCreatedByFetchEvent();

    nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(globalObj.GetAsSupports());
    if (NS_WARN_IF(!global)) {
      return false;
    }
    RefPtr<Request> request = new Request(global, internalReq);

    MOZ_ASSERT_IF(internalReq->IsNavigationRequest(),
                  request->Redirect() == RequestRedirect::Manual);

    RootedDictionary<FetchEventInit> init(aCx);
    init.mRequest = request;
    init.mBubbles = false;
    init.mCancelable = true;
    if (!mClientId.IsEmpty()) {
      init.mClientId = mClientId;
    }
    init.mIsReload = mIsReload;
    RefPtr<FetchEvent> event =
      FetchEvent::Constructor(globalObj, NS_LITERAL_STRING("fetch"), init, result);
    if (NS_WARN_IF(result.Failed())) {
      result.SuppressException();
      return false;
    }

    event->PostInit(mInterceptedChannel, mRegistration, mScriptSpec);
    event->SetTrusted(true);

    nsresult rv2 =
      DispatchExtendableEventOnWorkerScope(aCx, aWorkerPrivate->GlobalScope(),
                                           event, nullptr);
    if (NS_WARN_IF(NS_FAILED(rv2)) || !event->WaitToRespond()) {
      nsCOMPtr<nsIRunnable> runnable;
      MOZ_ASSERT(!aWorkerPrivate->UsesSystemPrincipal(),
                 "We don't support system-principal serviceworkers");
      if (event->DefaultPrevented(CallerType::NonSystem)) {
        runnable = new CancelChannelRunnable(mInterceptedChannel,
                                             mRegistration,
                                             NS_ERROR_INTERCEPTION_FAILED);
      } else {
        runnable = new ResumeRequest(mInterceptedChannel);
      }

      MOZ_ALWAYS_SUCCEEDS(mWorkerPrivate->DispatchToMainThread(runnable.forget()));
    }

    return true;
  }

  nsresult
  HandleBodyWithHeaders(nsIInputStream* aUploadStream)
  {
    // We are dealing with an nsMIMEInputStream which uses string input streams
    // under the hood, so all of the data is available synchronously.
    bool nonBlocking = false;
    nsresult rv = aUploadStream->IsNonBlocking(&nonBlocking);
    NS_ENSURE_SUCCESS(rv, rv);
    if (NS_WARN_IF(!nonBlocking)) {
      return NS_ERROR_NOT_AVAILABLE;
    }
    nsAutoCString body;
    rv = NS_ConsumeStream(aUploadStream, UINT32_MAX, body);
    NS_ENSURE_SUCCESS(rv, rv);

    // Extract the headers in the beginning of the buffer
    nsAutoCString::const_iterator begin, end;
    body.BeginReading(begin);
    body.EndReading(end);
    const nsAutoCString::const_iterator body_end = end;
    nsAutoCString headerName, headerValue;
    bool emptyHeader = false;
    while (FetchUtil::ExtractHeader(begin, end, headerName,
                                    headerValue, &emptyHeader) &&
           !emptyHeader) {
      mHeaderNames.AppendElement(headerName);
      mHeaderValues.AppendElement(headerValue);
      headerName.Truncate();
      headerValue.Truncate();
    }

    // Replace the upload stream with one only containing the body text.
    nsCOMPtr<nsIStringInputStream> strStream =
      do_CreateInstance(NS_STRINGINPUTSTREAM_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
    // Skip past the "\r\n" that separates the headers and the body.
    ++begin;
    ++begin;
    body.Assign(Substring(begin, body_end));
    rv = strStream->SetData(body.BeginReading(), body.Length());
    NS_ENSURE_SUCCESS(rv, rv);
    mUploadStream = strStream;

    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS_INHERITED(FetchEventRunnable, WorkerRunnable, nsIHttpHeaderVisitor)

} // anonymous namespace

END_WORKERS_NAMESPACE
