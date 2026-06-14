const express = require('express');

const GUI_REFRESH_INTERVAL_SNIPPET = '      setInterval(refreshState, 1000);';
const GUI_MANUAL_RUN_INPUT_PATCH_MARKER = '// Manual run input persistence patch';
const GUI_MANUAL_RUN_INPUT_PATCH = `      ${GUI_MANUAL_RUN_INPUT_PATCH_MARKER}
      const preserveManualRunInputValues = (renderFn) => function renderWithManualRunInputPersistence(nextState) {
        const relayGridBeforeRender = document.getElementById('relay-grid');
        const activeElement = document.activeElement;
        const activeManualInput = activeElement instanceof HTMLInputElement &&
          activeElement.name === 'minutes' &&
          Boolean(activeElement.closest('#relay-grid form[action^="/gui/relays/"][action$="/on"]'));
        const focusedFormAction = activeManualInput ? activeElement.closest('form').getAttribute('action') : null;
        const selectionStart = activeManualInput ? activeElement.selectionStart : null;
        const selectionEnd = activeManualInput ? activeElement.selectionEnd : null;
        const manualRunValues = new Map();

        if (relayGridBeforeRender) {
          for (const input of relayGridBeforeRender.querySelectorAll('form[action^="/gui/relays/"][action$="/on"] input[name="minutes"]')) {
            const form = input.closest('form');
            const formAction = form ? form.getAttribute('action') : null;
            if (formAction) {
              manualRunValues.set(formAction, input.value);
            }
          }
        }

        renderFn(nextState);

        const relayGridAfterRender = document.getElementById('relay-grid');
        if (!relayGridAfterRender) {
          return;
        }

        for (const input of relayGridAfterRender.querySelectorAll('form[action^="/gui/relays/"][action$="/on"] input[name="minutes"]')) {
          const form = input.closest('form');
          const formAction = form ? form.getAttribute('action') : null;
          if (formAction && manualRunValues.has(formAction)) {
            input.value = manualRunValues.get(formAction);
          }

          if (formAction === focusedFormAction) {
            input.focus({ preventScroll: true });
            try {
              if (selectionStart !== null && selectionEnd !== null) {
                input.setSelectionRange(selectionStart, selectionEnd);
              }
            } catch (_error) {
              // Number inputs do not support selection ranges in all browsers.
            }
          }
        }
      };
      renderFromState = preserveManualRunInputValues(renderFromState);
`;

function installGuiManualRunInputPatch() {
  const originalSend = express.response.send;
  if (originalSend.__gardenManualRunInputPatchInstalled) {
    return;
  }

  function sendWithGuiManualRunInputPatch(body) {
    if (
      typeof body === 'string' &&
      this.req &&
      this.req.path === '/gui' &&
      body.includes(GUI_REFRESH_INTERVAL_SNIPPET) &&
      !body.includes(GUI_MANUAL_RUN_INPUT_PATCH_MARKER)
    ) {
      return originalSend.call(
        this,
        body.replace(GUI_REFRESH_INTERVAL_SNIPPET, `${GUI_MANUAL_RUN_INPUT_PATCH}${GUI_REFRESH_INTERVAL_SNIPPET}`)
      );
    }

    return originalSend.call(this, body);
  }

  sendWithGuiManualRunInputPatch.__gardenManualRunInputPatchInstalled = true;
  express.response.send = sendWithGuiManualRunInputPatch;
}

installGuiManualRunInputPatch();

const { createApp } = require('./app');

const PORT = process.env.PORT || 3000;
const { app } = createApp();

app.listen(PORT, () => {
  // eslint-disable-next-line no-console
  console.log(`Garden Controller listening on port ${PORT}`);
});
