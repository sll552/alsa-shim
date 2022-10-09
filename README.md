# alsa-shim

This alsa plugin adds the possibility to execute a command before the alsa device is opened or closed. The plugin does not change the actual pcm data in any way and is therefore transparent.

## Example config

```text
pcm.shim {
  type shim
  slave.pcm _audioout
  open_hook {
    path /home/pi/test.sh
    blocking 1
  }
  close_hook {
    path /home/pi/test.sh
    blocking 0
  }
}
```

This configuration runs the command `/home/pi/test.sh` before the alsa device is opened and waits for it's completion before continuing. When the alsa device is closed, the same command is executed but this time without waiting for it's completion. During playback all audio is forwarded as is to the slave device, in this case `_audioout`.
