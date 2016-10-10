/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceParent_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceParent_h


namespace mozilla {
namespace dom {

namespace workers {

class ServiceWorkerPrivate;

/**
 * Abstraction layer for ServiceWorkerPrivate to send events to the child
 * process and receive some asynchronous notifications like errors.  All
 * serialization/deserialization happens here.
 *
 * The lifecycle of this class is entirely that of the actor.  When constructed
 * on the main thread, a ServiceWorkerPrivate is provided that holds a
 * non-reference-counted pointer to us.  When our actor is destroyed, we
 * synchronously notify the ServiceWorkerPrivate which must immediately null out
 * its pointer.  It will also drop the reference to us when it invokes
 * Terminate.
 */
class ServiceWorkerInstanceParent final : public PServiceWorkerInstanceParent
{

public:
  ServiceWorkerInstanceParent(ServiceWorkerPrivate* aOwner);

  nsresult
  DispatchMessageEvent(JSContext* aCx, JS::Handle<JS::Value> aMessage,
                       const Optional<Sequence<JS::Value>>& aTransferable,
                       UniquePtr<ServiceWorkerClientInfo>&& aClientInfo);

  // This is used to validate the worker script and continue the installation
  // process.
  nsresult
  DispatchCheckScriptEvaluation(LifeCycleEventCallback* aCallback);

  nsresult
  DispatchSendLifeCycleEvent(const nsAString& aEventType,
                             LifeCycleEventCallback* aCallback,
                             nsIRunnable* aLoadFailure);

  nsresult
  DispatchPushEvent(const nsAString& aMessageId,
                    const Maybe<nsTArray<uint8_t>>& aData,
                    ServiceWorkerRegistrationInfo* aRegistration);

  nsresult
  DispatchPushSubscriptionChangeEvent();

  nsresult
  DispatchNotificationEvent(const nsAString& aEventName,
                            const nsAString& aID,
                            const nsAString& aTitle,
                            const nsAString& aDir,
                            const nsAString& aLang,
                            const nsAString& aBody,
                            const nsAString& aTag,
                            const nsAString& aIcon,
                            const nsAString& aData,
                            const nsAString& aBehavior,
                            const nsAString& aScope);

  nsresult
  DispatchFetchEvent(nsIInterceptedChannel* aChannel,
                     nsILoadGroup* aLoadGroup,
                     const nsAString& aDocumentId,
                     bool aIsReload);


  /**
   * Terminate the instance.  Caller must drop their (non-owning) reference to
   * us after calling.
   *
   * Pending events XXX TODO
   */
  void
  Terminate();



private:
  // PServiceWorkerInstance methods
  virtual bool
  RecvReportError();

  virtual PServiceWorkerEventParent*
  AllocPServiceWorkerEventParent(const ServiceWorkerEventArgs& args);

  virtual bool
  DeallocPServiceWorkerEventParent(PServiceWorkerEventParent* aActor);

  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  /**
   * The ServiceWorkerPrivate that owns and manages us.  The lifecycle mirrors
   * that of the ServiceWorkerPrivate's mirroring non-owning
   * mServiceWorkerInstance reference.  Established in our constructor and
   * dropped when Terminate is called on us or we get `ActorDestroy`ed.  Exists
   * so we can notify mOwner in the `ActorDestroy` case.
   */
  ServiceWorkerPrivate* MOZ_NON_OWNING_REF mOwner;
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceParent_h
