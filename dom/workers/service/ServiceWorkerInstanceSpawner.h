/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_service_ServiceWorkerInstanceSpawner_h
#define mozilla_dom_workers_service_ServiceWorkerInstanceSpawner_h

#include "nsIObserver.h"
#include "nsString.h"

class nsIPrincipal;

namespace mozilla {
namespace dom {

class ContentParent;

namespace workers {

/**
 * Static API for ServiceWorkerPrivates to spawn instances in our
 * dedicated-for-serviceworkers child content process.  In the future multiple
 * content processes may be involved, but if they aren't dedicated to Service
 * Workers then the bookkeeping for that will likely end up in ContentParent,
 * not here.
 *
 * On startup we acquire a dedicated ContentParent of our very own to host our
 * serviceworker instances.  We hold onto it until we are explicitly shutdown.
 * Startup is triggered by the first call to SpawnInstance() that happens.  For
 * now, this occurs the first time a ServiceWorkerPrivate wants to spawn a
 * ServiceWorker.  As long as the preallocated process mechanism is enabled,
 * this should not be too bad.
 *
 * Shutdown is triggered explicitly by the ServiceWorkerManager in order to
 * provide ServiceWorkers with a coherent shutdown boundary.  Once triggered,
 * all calls to SpawnInstance() will fail.
 *
 * In the event of (abnormal) termination of our content process, we learn about
 * it via "ipc:content-shutdown" observer notification.  Alternately, we could
 * have introduced an additional protocol so that we could directly receive
 * an ActorDestroy notification on it.
 *
 * Internally the static API is implemented via singleton that we hide from
 * callers to slightly reduce caller boilerplate.  The class does need to exist
 * because we implement nsIObserver.
 */
class ServiceWorkerInstanceSpawner final : public nsIObserver
{
public:
  /**
  * Create a ServiceWorkerInstanceChild in a content process and return the
  * ServiceWorkerInstanceParent to use to send events to the instance.
  *
  * Currently all instances will be spawned in a single content process child.
  * In the future, this will change.
  *
  * This method will fail and return a null parent if Shutdown() has already
  * been invoked.
  */
  static already_AddRefed<ServiceWorkerInstanceParent>
  SpawnInstance(nsIPrincipal* aPrincipal,
                const nsACString& aScope,
                const nsACString& aScriptSpec,
                const nsAString& aCacheName);

  /**
   * Called by ServiceWorkerManager during shutdown to indicate that the
   * dedicated content process should be shutdown if it's alive and
   * SpawnInstance should return null without doing any work.
   *
   * No effort is made to wait for pending events to complete.
   */
  static void
  Shutdown();

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

private:
  ServiceWorkerInstanceSpawner();
  ~ServiceWorkerInstanceSpawner();

  /**
   * Create the ContentParent (if possible).
   */
  void
  Init();

  /**
   * Actual implementation of our static SpawnInstance; the static one just
   * avoids singleton-related boilerplate for the caller.
   */
  already_AddRefed<ServiceWorkerInstanceParent>
  Spawn(nsIPrincipal* aPrincipal,
        const nsACString& aScope,
        const nsACString& aScriptSpec,
        const nsAString& aCacheName);

  /**
   * Shutdown the ContentParent if we have one.
   */
  void
  Shutdown();

  RefPtr<ContentParent> mDedicatedServiceWorkerProcess;

  static uint64_t sNextID;
};


} // namespace workers
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_workers_service_ServiceWorkerInstanceSpawner_h
