import { For, Show, type Component } from 'solid-js';

export const ChatHelpMessage: Component<{ text: string }> = (props) => (
  <>
    <div class="flex items-center gap-2 pb-2">
      <img src="/favicon.ico" alt="" width="20" height="20" class="h-5 w-5" />
      <strong>ESP-Claw /help</strong>
    </div>
    <p class="m-0 whitespace-pre-wrap break-words">
      <For each={props.text.replace(/^ESP-Claw \/help\n/, '').split('\n')}>
        {(line, index) => {
          const address = /^管理地址 (http:\/\/[\d.]+\/)$/.exec(line)?.[1];
          return (
            <>
              {index() > 0 ? '\n' : ''}
              <Show when={address} fallback={line}>
                {(url) => <>管理地址 <a class="underline" href={url()}>{url()}</a></>}
              </Show>
            </>
          );
        }}
      </For>
    </p>
  </>
);
