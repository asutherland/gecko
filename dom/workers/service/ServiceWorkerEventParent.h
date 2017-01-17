/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerEventParent_h
#define mozilla_dom_workers_service_ServiceWorkerEventParent_h

#include "mozilla/dom/PServiceWorkerEventParent.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Variant.h"
#include "nsCOMPtr.h"

class LifeCycleEventCallback;
class nsIInterceptedChannel;

namespace mozilla {
namespace dom {

namespace workers {

/**
 * Holds event callbacks and acts like[1] a KeepAliveToken for
 * ServiceWorkerPrivate.  ServiceWorkerPrivate does no other per-event
 * bookkeeping.
 *
 * Because this single actor is used for all event types, there's a bit of
 * multiplexing that occurs.  Our idiom is to have specific logic for each event
 * type Foo as follows:
 * - CreateFoo constructor method to instantiate the actor.
 * - DoneFoo method to handle the result payload.
 * - FooStorage struct type, used in our mEventStorage Variant<> type to store
 *   things like callbacks or information that we cannot trust the content
 *   process to repeat back to us (or would just be inefficient to roundtrip).
 * Alternately, we could have implemented a class hierarchy for callbacks,
 * expanding FooEventStorage to be a full class that implements a common Done()
 * method.  That may be the way to go in the future if this implementation
 * starts to prove ungainly.
 *
 * 1: "Acts like" a KeepAliveToken for visibility reasons and because we don't
 * need it; our life-cycle lines up exactly with that of a would-be token and we
 * live exclusively on the main thread.  (KeepAliveToken pre-remoting was held
 * by worker-thread-exposed objects using nsMainThreadPtrHandle with asserts
 * ensuring the release was main-thread.)
 */
class ServiceWorkerEventParent final : public PServiceWorkerEventParent
{
  struct EvaluateScriptStorage {};

  struct LifeCycleStorage
  {
    nsRefPtr<LifeCycleEventCallback> callback;
  };

  struct FetchStorage
  {
    nsCOMPtr<nsIInterceptedChannel> interceptedChannel;
  };

  struct PostMessageStorage {};

  struct PushStorage {
    nsString messageId;
  };

  struct PushSubscriptionChangeStorage {};

  struct NotificationStorage {};

  using EventStorage = Variant<EvaluateScriptStorage, LifeCycleStorage,
                               FetchStorage, PostMessageStorage, PushStorage,
                               PushSubscriptionChangeStorage,
                               NotificationStorage>;

public:
  static ServiceWorkerEventParent*
  CreateEvaluateScript(ServiceWorkerPrivate* aOwner);

  static ServiceWorkerEventParent*
  CreateLifeCycle(ServiceWorkerPrivate* aOwner,
                  LifeCycleEventCallback* aCallback);

  static ServiceWorkerEventParent*
  CreateFetchEvent(ServiceWorkerPrivate* aOwner,
                   nsIInterceptedChannel* aIntercepted);

  static ServiceWorkerEventParent*
  CreatePostMessage(ServiceWorkerPrivate* aOwner);

  static ServiceWorkerEventParent*
  CreatePush(ServiceWorkerPrivate* aOwner);

  static ServiceWorkerEventParent*
  CreatePushSubscriptionChange(ServiceWorkerPrivate* aOwner);

  static ServiceWorkerEventParent*
  CreateNotification(ServiceWorkerPrivate* aOwner);

  virtual ~ServiceWorkerEventParent();

private:
  explicit ServiceWorkerEventParent(ServiceWorkerPrivate* aOwner,
                                    ServiceWorkerEventArgs::Type aType,
                                    EventStorage&& aStorage);

  // PServiceWorkerEvent methods
  virtual bool
  RecvFetchEventRespondWith(const IPCInternalResponse& aResponse) override;

  // Calls out to the relevant per-event-type Done* method to avoid a horrible
  // if-cascade with many ridiculously verbose types.
  virtual bool
  Recv__delete__(const ServiceWorkerEventResult& aResult) override;

  // For any reason other than deletion, treat this as a failure.  Either way,
  // release the notional ServiceWorkerPrivate keepalive token.
  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  void
  DoneEvaluateScript(const ServiceWorkerEvaluateScriptEventResult& aResult);

  void
  DoneLifeCycle(const ServiceWorkerLifeCycleEventResult& aResult);

  void
  DoneFetchEvent(const ServiceWorkerFetchEventResult& aResult);

  void
  DonePostMessage(const ServiceWorkerPostMessageEventResult& aResult);

  void
  DonePush(const ServiceWorkerPushEventResult& aResult);

  void
  DonePushSubscriptionChange(const ServiceWorkerPushSubscriptionChangeEventResult& aResult);

  void
  DoneNotification(const ServiceWorkerNotificationEventResult& aResult);

  // Post-functional-event time-check.
  void
  CommonDoneFunctionalEvent();

  nsRefPtr<ServiceWorkerPrivate> mOwner;
  // The type of the event we correspond to so that we can ensure the result
  // union type appropriately matches.
  ServiceWorkerEventArgs::Type mEventType;
  // Per-event storage data.  Its tag type is redundant with mEventType for now.
  EventStorage mEventStorage;
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerEventParent_h
