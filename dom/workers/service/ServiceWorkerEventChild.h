/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerEventChild_h
#define mozilla_dom_workers_service_ServiceWorkerEventChild_h


namespace mozilla {
namespace dom {

namespace workers {

/**
 * Home to the runnables/logic for servicing the events; our owning
 * ServiceWorkerInstanceChild owns the WorkerPrivate.  We are similarly
 * reference-counted like it because of all the runnables involved in the use
 * of workers.
 */
class ServiceWorkerEventChild final : public PServiceWorkerEventChild
                                    , public nsISupports
{
public:
  ServiceWorkerEventChild(ServiceWorkerInstanceChild *aOwner);
  ~ServiceWorkerEventChild();

  // Process the event request by dispatching to the event-type-specific Start*
  // methods.  This begs the question of why we are a generic EventChild rather
  // than having specific EventTYPEChild classes and our answer is to reduce
  // proliferation of actor types because they're a hassle.
  void
  Init(const ServiceWorkerEventArgs &aArgs);


private:
  // PServiceWorkerInstance methods
  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  void
  StartEvaluateScript(const ServiceWorkerEvaluateScriptEventArgs &aArgs);

  void
  StartLifeCycle(const ServiceWorkerLifeCycleEventArgs &aArgs);

  void
  StartFetchEvent(const ServiceWorkerFetchEventArgs &aArgs);

  // Implements 2-step handling of the received Structured Clone:
  // 1) On the main thread, unpack the serialized IPC structured clone, writing
  //    it/transferring ownership into a holder.  Dispatch to worker thread.
  // 2) On the worker thread, perform the actual structured clone reading
  //    including creating the actors for blobs and finalizing the transfer /
  //    entangling of message ports.
  //
  // This is currently a somewhat unique situation because in other structured
  // cloning and postMessage cases the communication is either bouncing across
  // threads or across processes where the IPC call is already delivering to
  // the correct target thread.  That is, for MessageChannel/MessagePort, the
  // actor receiving the message is already on the worker thread (via
  // PBackground), the message does not need to bounce off the main thread.
  void
  StartPostMessage(const ServiceWorkerPostMessageEventArgs &aArgs);

  // Necessary for access to the WorkerPrivate.
  RefPtr<ServiceWorkerInstanceChild> mOwner;

public:
  NS_DECL_ISUPPORTS

  void
  DoneEvaluateScript(bool aResult);

  void
  DoneLifeCycle(bool aResult);

  void
  DoneFetchEvent();

  void
  DonePostMessage();
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerEventChild_h
