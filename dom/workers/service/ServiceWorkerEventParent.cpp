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
  EventStorage&& aStorage)
  : mOwner(aOwner)
  , mType(aType)
  , mStorage(aStorage)
{
  MOZ_COUNT_CTOR(ServiceWorkerEventParent);
  mOwner->AddToken();
}

/* static*/ ServiceWorkerEventParent*
ServiceWorkerEventParent::CreateEvaluateScript(ServiceWorkerPrivate* aOwner)
{
  return new ServiceWorkerEventParent(
    aOwner,
    ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs,
    EvaluateScriptStorage {});
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
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerPostMessageEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerPushEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerPushEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerPushSubscriptionChangeEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerPushSubscriptionChangeEventResult) ||
        (mEventType == ServiceWorkerEventArgs::TServiceWorkerNotificationEventArgs &&
         aResult.type() == ServiceWorkerEventResult::TServiceWorkerNotificationEventResult))) {
    MOZ_CRASH("ServiceWorkerEventResult type mismatch.");
  }

  switch (mEventType) {
    case ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs:
      DoneEvaluateScript(aResult.ServiceWorkerEvaluateScriptEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerLifeCycleEventArgs:
      DoneLifeCycle(aResult.ServiceWorkerLifeCycleEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs:
      DoneFetchEvent(aResult.ServiceWorkerFetchEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPostMessageEventArgs:
      DonePostMessage(aResult.ServiceWorkerPostMessageEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPushEventArgs:
      DonePush(aResult.ServiceWorkerPushEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerPushSubscriptionChangeEventArgs:
      DonePushSubscriptionChange(aResult.ServiceWorkerPushSubscriptionChangeEventResult());
      break;
    case ServiceWorkerEventArgs::TServiceWorkerNotificationEventArgs:
      DoneNotification(aResult.ServiceWorkerNotificationEventResult());
      break;
  }

  if (aResult.type() == ServiceWorkerEventResult::TServiceWorkerEvaluateScriptEventResult) {
  } else if (aResult.type() == ServiceWorkerEventResult::TServiceWorkerLifeCycleEventResult) {
    mCallback->SetResult(.result);
    Unused << mCallback->Run();
    mCallback = nullptr;
  } else {
    MOZ_ASSERT(!mCallback);
  }
}

void
ServiceWorkerEventParent::ActorDestroy(ActorDestroyReason aReason)
{
  // If we are being destroyed for any reason other than explicit deletion, then
  // explicitly treat this as a failure.
  if (aReason != Deletion) {
    switch (mEventType) {
      case ServiceWorkerEventArgs::TServiceWorkerEvaluateScriptEventArgs:
        DoneEvaluateScript(ServiceWorkerEvaluateScriptEventResult(false));
        break;
      case ServiceWorkerEventArgs::TServiceWorkerLifeCycleEventArgs:
        DoneLifeCycle(ServiceWorkerLifeCycleEventResult(false));
        break;
      case ServiceWorkerEventArgs::TServiceWorkerFetchEventArgs:
        DoneFetchEvent(ServiceWorkerFetchEventResult());
        break;
      case ServiceWorkerEventArgs::TServiceWorkerPostMessageEventArgs:
        DonePostMessage(ServiceWorkerPostMessageEventResult());
        break;
      case ServiceWorkerEventArgs::TServiceWorkerPushEventArgs:
        DonePush(ServiceWorkerPushEventResult());
        break;
      case ServiceWorkerEventArgs::TServiceWorkerPushSubscriptionChangeEventArgs:
        DonePushSubscriptionChange(ServiceWorkerPushSubscriptionChangeEventResult());
        break;
      case ServiceWorkerEventArgs::TServiceWorkerNotificationEventArgs:
        DoneNotification(ServiceWorkerNotificationEventResult());
        break;
    }
  }
  // If we still have a callback, then call it notifying failure.
  if (mCallback) {
    mCallback->SetResult(false);
    Unused << mCallback->Run();
    mCallback = nullptr;
  }

  mOwner->ReleaseToken();
  mOwner = nullptr;
}

void
ServiceWorkerEventParent::DoneEvaluateScript(
  const ServiceWorkerEvaluateScriptEventResult& aResult)
{
  // LifeCycleEventCallbacks are main-thread runnables and so we can
  // synchronously invoke them as we are defined to be on the main thread.
  mCallback->SetResult(aResult.result());
  Unused << mCallback->Run();
  mCallback = nullptr;
}

void
ServiceWorkerEventParent::DoneLifeCycle(
  const ServiceWorkerLifeCycleEventResult& aResult)
{
  // LifeCycleEventCallbacks are main-thread runnables and so we can
  // synchronously invoke them as we are defined to be on the main thread.
  mCallback->SetResult(aResult.result());
  Unused << mCallback->Run();
  mCallback = nullptr;
}

void
ServiceWorkerEventParent::DoneFetchEvent(
  ServiceWorkerFetchEventResult& aResult)
{
  CommonDoneFunctionalEvent();
}

void
ServiceWorkerEventParent::DonePostMessage(
  ServiceWorkerPostMessageEventResult& aResult)
{
  // Not a functional event.
}

void
ServiceWorkerEventParent::DonePush(
  ServiceWorkerPushEventResult& aResult)
{
  uint16_t reason = aResult.reason();
  if (reason) {
    nsCOMPtr<nsIPushErrorReporter> reporter =
      do_GetService("@mozilla.org/push/Service;1");
    if (reporter) {
      nsresult rv = reporter->ReportDeliveryError(mMessageId,reason);
      Unused << NS_WARN_IF(NS_FAILED(rv));
    }
  }

  CommonDoneFunctionalEvent();
}

void
ServiceWorkerEventParent::DonePushSubscriptionChange(
  ServiceWorkerPushSubscriptionChangeEventResult& aResult)
{
  CommonDoneFunctionalEvent();
}

void
ServiceWorkerEventParent::DoneNotification(
  ServiceWorkerNotificationEventResult& aResult)
{
  CommonDoneFunctionalEvent();
}

void
ServiceWorkerEventParent::CommonDoneFunctionalEvent()
{
  mOwner->FunctionalEventUpdateCheck();
}


END_WORKERS_NAMESPACE
