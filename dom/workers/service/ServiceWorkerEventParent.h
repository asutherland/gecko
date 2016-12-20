/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerEventParent_h
#define mozilla_dom_workers_service_ServiceWorkerEventParent_h

class nsIInterceptedChannel;

namespace mozilla {
namespace dom {

namespace workers {

/**
 * Holds event callbacks and acts like a KeepAliveToken for
 * ServiceWorkerPrivate.  ServiceWorkerPrivate does no other per-event
 * bookkeeping.
 *
 * "Acts like" a KeepAliveToken for visibility reasons and because we don't need
 * it; our life-cycle lines up exactly with that of a would-be token and we live
 * exclusively on the main thread.  (KeepAliveToken pre-remoting was held by
 * worker-thread-exposed objects using nsMainThreadPtrHandle with asserts
 * ensuring the release was main-thread.)
 */
class ServiceWorkerEventParent final : public PServiceWorkerEventParent
{

public:
  /**
   * @param aCallback
   *   Provided for script evaluation and lifecycle events.
   */
  explicit ServiceWorkerEventParent(ServiceWorkerPrivate* aOwner,
                                    ServiceWorkerEventArgs::Type aType,
                                    LifeCycleEventCallback* aCallback,
                                    nsIInterceptedChannel* aIntercepted);
  virtual ~ServiceWorkerEventParent();

private:
  // PServiceWorkerEvent methods
  virtual bool
  RecvFetchEventRespondWith(const IPCInternalResponse& aResponse) override;

  // Calls out to the relevant per-event-type Done* method to avoid a horrible
  // if-cascade with many ridiculously verbose types.
  virtual bool
  Recv__delete__(const ServiceWorkerEventResult& aResult) override;

  // Releases the ServiceWorkerPrivate token and performs any failsafe
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

  nsRefPtr<ServiceWorkerPrivate> mOwner;
  // At most one of mCallback, mInterceptedChannel will be non-null depending on
  // mEventType.
  nsRefPtr<LifeCycleEventCallback> mCallback;
  nsCOMPtr<nsIInterceptedChannel> mInterceptedChannel;
  // The type of the event we correspond to so that we can ensure the result
  // union type appropriately matches.
  ServiceWorkerEventArgs::Type mEventType;
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerEventParent_h
