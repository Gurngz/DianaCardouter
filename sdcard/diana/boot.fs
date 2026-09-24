\ DIANA boot script - runs at every boot after config and audio are up.
\ One Forth line per line (max 250 chars). Lines starting with \ are comments.
\ Edit on a laptop, reinsert the card, reboot - or type /fs boot on the HUD.
\ See README.md "Forth scripting" and type /forth diana-words for the vocabulary.

\ --- tuning examples (uncomment to use) ---------------------------------------
\ s" vad" 700 tune!              \ hears less room noise (default 550)
\ s" silence_ms" 900 tune!       \ ends a recording sooner after you stop talking
\ s" stream_chunk" 3200 tune!    \ deeper playback cushion on a slow network
\ s" reply_tokens" 160 tune!     \ shorter, faster replies
\ cfg-save                       \ persist vad/silence/mic/volume/brightness to config

\ --- a word of your own -------------------------------------------------------
: hello  s" Forth online." log ;
hello
