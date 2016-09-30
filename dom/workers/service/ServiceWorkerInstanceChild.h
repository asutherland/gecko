/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceChild_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceChild_h

#include "mozilla/dom/PServiceWorkerInstanceChild.h"
#include "nsISupports.h"

namespace mozilla {
namespace dom {

namespace workers {

/**
 * Creates and holds a ServiceWorker WorkerPrivate active for remote events to
 * be dispatched to it.  The ServiceWorkerEventChild implementation is where the
 * event arguments are interpreted and converted into the runnables that
 * interact with the worker thread.
 *
 * The involvement of the worker thread and runnables necessitates the use of
 * reference counting for this class and ServiceWorkerEventChild.  (Alternately,
 * a separate reference-counted class could have been introduced with a nullable
 * pointer to our instance rather than a mActorDestroyed boolean, but we gain
 * little from the extra indirection.)
 */
class ServiceWorkerInstanceChild final : public PServiceWorkerInstanceChild
                                       , public nsISupports
{
public:
  void
  Init(const ServiceWorkerInstanceConfig& aConfig);

  // For use by ServiceWorkerEventChild
  class WorkerPrivate*
  WorkerPrivate() const
  {
    MOZ_ASSERT(mWorkerPrivate);
    return mWorkerPrivate;
  }

private:
  // PServiceWorkerInstance methods
  virtual PServiceWorkerEventChild*
  AllocPServiceWorkerEventChild(const ServiceWorkerEventArgs& aArgs) override;

  virtual bool
  DeallocPServiceWorkerEventChild(PServiceWorkerEventChild* aActor) override;

  virtual bool
  RecvPServiceWorkerEventChildConstructor(PServiceWorkerEventChild* aActor,
                                          const ServiceWorkerEventArgs &aArgs) override;

  virtual void
  ActorDestroy(ActorDestroyReason aReason) override;

  MOZ_IMPLICIT ServiceWorkerInstanceChild();
  virtual ~ServiceWorkerInstanceChild();

  // Template-magicked to be the worker LoadFailedAsyncRunnable handler to
  // __delete__ us if we failed to spin up the worker correctly.
  void
  WorkerLoadFailedDeleteSelf();


  // Has ActorDestroy() been invoked?  Gates IPC calls given the async nature of
  // the worker thread and our interaction with it.
  bool mActorDestroyed;

  // The WorkerPrivate object can only be closed by this class or by the
  // RuntimeService class if gecko is shutting down. Closing the worker
  // multiple times is OK, since the second attempt will be a no-op.
  RefPtr<WorkerPrivate> mWorkerPrivate;

public:
  NS_DECL_ISUPPORTS
};

} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceChild_h
