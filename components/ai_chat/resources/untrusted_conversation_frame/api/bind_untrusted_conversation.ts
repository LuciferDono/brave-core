// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as Mojom from '../../common/mojom'
import createUntrustedConversationApi from './untrusted_conversation_api'

export async function bindUntrustedConversation() {
  // Create remotes
  const conversationHandler = new Mojom.UntrustedConversationHandlerRemote()
  const uiHandler = Mojom.UntrustedUIHandler.getRemote()
  const parentUIFrame = new Mojom.ParentUIFrameRemote()
  // Service is bound directly to the WebUI via the interface broker
  const service = Mojom.UntrustedService.getRemote()

  // Get conversation ID from URL
  const conversationId = window.location.pathname.split('/').pop() || ''

  // Bind conversation handler
  uiHandler.bindConversationHandler(
    conversationId,
    conversationHandler.$.bindNewPipeAndPassReceiver(),
  )

  // Set up communication with the parent frame
  uiHandler.bindParentPage(parentUIFrame.$.bindNewPipeAndPassReceiver())

  // Create the API
  const conversationAPI = createUntrustedConversationApi(
    conversationHandler,
    uiHandler,
    parentUIFrame,
    service,
  )

  // Bind UntrustedUI events
  const uiReceiver = new Mojom.UntrustedUIReceiver(conversationAPI.uiObserver)
  uiHandler.bindUntrustedUI(uiReceiver.$.bindNewPipeAndPassRemote())

  // Bind the conversation observer and get initial state
  const conversationUIReceiver = new Mojom.UntrustedConversationUIReceiver(
    conversationAPI.conversationObserver,
  )
  const { conversationEntriesState } =
    await conversationHandler.bindUntrustedConversationUI(
      conversationUIReceiver.$.bindNewPipeAndPassRemote(),
    )

  // Bind the service observer and get initial service state
  const serviceObserverReceiver = new Mojom.UntrustedServiceObserverReceiver(
    conversationAPI.serviceObserver,
  )
  const { state: serviceState } = await service.bindObserver(
    serviceObserverReceiver.$.bindNewPipeAndPassRemote(),
  )

  // Set initial state
  // Emit the event instead of directly updating so that any custom
  // handling (e.g. model filtering) happens and we don't need to duplicate
  // here.
  conversationAPI.api.emitEvent('onEntriesUIStateChanged', [
    conversationEntriesState,
  ])

  // Set initial service state
  conversationAPI.api.emitEvent('onStateChanged', [serviceState])

  // Note: Height reporting to parent frame has been removed since the iframe
  // now manages its own scrolling via ScrollableContent.

  return {
    api: conversationAPI.api,
    close: () => {
      conversationAPI.close()
      conversationUIReceiver.$.close()
      uiReceiver.$.close()
      serviceObserverReceiver.$.close()
    },
  }
}

export type BoundUntrustedConversation = Awaited<
  ReturnType<typeof bindUntrustedConversation>
>
