# dmenu_scratch
Tool to manage i3wm scratchpad using dmenu as frontend.

> [!NOTE]  
> dmenu_scratchpad development has been moved to [codeberg repo](https://codeberg.org/c_nicnic/dmenu_scratchpad).

## Notes
- This tool only works if you are using [i3](https://i3wm.org/) as your window manager.
- You will need [dmenu](https://tools.suckless.org/dmenu/) which usually comes by default in an i3 instalation.
- Additionally when scratchpad is empty it will attempt to use [dunstify](https://github.com/dunst-project/dunst) to show a desktop notification (should also come with i3 instalation), it's not obligatory, but you won't get any notifications without it.

## Quick start
```console
$ ./build.sh
$ ./dmenu_scratch
```

## Integrating with i3
You can add something like the following line to your i3 config file (usually located at `~/.config/i3`):
```
bindsym $mod+KEY exec --no-startup-id /PATH/TO/dmenu_scratch
```
