/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

ServiceWorkerEventParent::ServiceWorkerEventParent(
  ServiceWorkerPrivate* aOwner,
  ServiceWorkerEventArgs::Type aType,
  LifeCycleEventCallback* aCallback,
  nsIInterceptedChannel* aIntercepted)
  : mOwner(aOwner)
  , mCallback(aCallback)
  , mInterceptedChannel(aIntercepted)
  , mType(aType)
{
  MOZ_COUNT_CTOR(ServiceWorkerEventParent);
  mOwner->AddToken();
}

bool
ServiceWorkerEventParent::RecvFetchEventRespondWith(
  const IPCInternalResponse& aResponse)
{
  if (mEventType != ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs) {
    MOZ_CRASH("ServiceWorkerEvent type mismatch.");
  }

  RefPtr<InternalResponse> ir = InternalResponse::FromIPC(aResponse);
  if (NS_WARN_IF(!ir)) {
    Cancel(NS_ERROR_INTERCEPTION_FAILED);
    return;
  }

}

bool
ServiceWorkerEventParent::Recv__delete__(const ServiceWorkerEventResult& aResult)
{
  // Require that the result type is both a valid type and that it corresponds
  // to the event arg type we sent.
  if (!((mEventType == ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerEvaluateScriptEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerLifeCycleEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerLifeCycleEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerFetchEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerPostMessageEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerFetchEventResult))) {
    MOZ_CRASH("ServiceWorkerEventResult type mismatch.");
  }

  // LifeCycleEventCallbacks are main-thread runnables and so we can
  // synchronously invoke them as we are defined to be on the main thread.
  if (aResult.type() == ServiceWorkerEventResult::TServiceWorkerEvaluateScriptEventResult) {
    mCallback->SetResult(aResult.ServiceWorkerEvaluateScriptEventResult().result);
    Unused << mCallback->Run();
    mCallback = nullptr;
  } else if (aResult.type() == ServiceWorkerEventResult::TServiceWorkerLifeCycleEventResult) {
    mCallback->SetResult(aResult.ServiceWorkerLifeCycleEventResult().result);
    Unused << mCallback->Run();
    mCallback = nullptr;
  } else {
    MOZ_ASSERT(!mCallback);
  }
}

void
ServiceWorkerEventParent::ActorDestroy(ActorDestroyReason aReason)
{
  mOwner->ReleaseToken();
  mOwner = nullptr;
}

END_WORKERS_NAMESPACE
