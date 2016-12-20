/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerInstanceParent.h"

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

ServiceWorkerInstanceParent::ServiceWorkerInstanceParent(
  ServiceWorkerPrivate* aOwner)
{
  MOZ_COUNT_CTOR(ServiceWorkerInstanceParent);
}

nsresult
ServiceWorkerInstanceParent::DispatchMessageEvent(
  JSContext* aCx, JS::Handle<JS::Value> aMessage,
  const Optional<Sequence<JS::Value>>& aTransferable,
  UniquePtr<ServiceWorkerClientInfo>&& aClientInfo)
{
  MOZ_COUNT_DTOR(ServiceWorkerInstanceParent);
}

nsresult
ServiceWorkerInstanceParent::DispatchCheckScriptEvaluation(
  LifeCycleEventCallback* aCallback)
{

}

nsresult
ServiceWorkerInstanceParent::DispatchSendLifeCycleEvent(const nsAString& aEventType,
                           LifeCycleEventCallback* aCallback,
                           nsIRunnable* aLoadFailure)
{

}

nsresult
ServiceWorkerInstanceParent::DispatchPushSubscriptionChangeEvent()
{

}

nsresult
ServiceWorkerInstanceParent::DispatchNotificationEvent(const nsAString& aEventName,
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

}

nsresult
ServiceWorkerInstanceParent::DispatchFetchEvent(nsIInterceptedChannel* aChannel,
                  nsILoadGroup* aLoadGroup,
                  const nsAString& aDocumentId,
                  bool aIsReload)
{

}

void
ServiceWorkerInstanceParent::Terminate()
{

}

void
ServiceWorkerInstanceParent::ActorDestroy(ActorDestroyReason aReason)
{

}

END_WORKERS_NAMESPACE
