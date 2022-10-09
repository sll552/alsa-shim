/*
MIT License

Copyright (c) 2022 sll552

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_plugin.h>
#include <alsa/pcm_extplug.h>
#include <alsa/conf.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

#define DEBUG

#ifdef DEBUG
#define TRACE(format, ...) fprintf(stderr, "%s:%d:(%s) " format, \
                                   __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define TRACE(format, ...)
#endif

typedef struct
{
  const char *path;
  bool blocking;
} snd_pcm_shim_hook_t;

typedef struct
{
  snd_pcm_extplug_t ext;
  snd_pcm_shim_hook_t open_hook;
  snd_pcm_shim_hook_t close_hook;
} snd_pcm_shim_t;

static inline void *area_addr(const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset)
{
  unsigned int bitofs = area->first + area->step * offset;
  return (char *)area->addr + bitofs / 8;
}

static void *run_system(void *arg)
{
  system((char *)arg);
  pthread_exit(NULL);
}

static int call_hook(snd_pcm_shim_hook_t *hook)
{
  const char *path = hook->path;
  bool blocking = hook->blocking;
  pthread_t thread;

  if (path == NULL || strlen(path) < 1)
  {
    TRACE("Path for hook was empty\n");
    return 0;
  }
  if (blocking)
  {
    TRACE("Running blocking hook '%s'\n", path);
    return system(path);
  }
  else
  {
    TRACE("Running non-blocking hook '%s'\n", path);
    return pthread_create(&thread, NULL, &run_system, (void *)path) && pthread_detach(thread);
  }
  return 0;
}

static snd_pcm_sframes_t
transfer_callback(snd_pcm_extplug_t *ext,
                  const snd_pcm_channel_area_t *dst_areas,
                  snd_pcm_uframes_t dst_offset,
                  const snd_pcm_channel_area_t *src_areas,
                  snd_pcm_uframes_t src_offset,
                  snd_pcm_uframes_t size)
{
  short *src = area_addr(src_areas, src_offset);
  short *dst = area_addr(dst_areas, dst_offset);

  // no processing
  memcpy(dst, src, size * 2);

  return size;
}

static int close_callback(snd_pcm_extplug_t *ext)
{
  snd_pcm_shim_t *shim = (snd_pcm_shim_t *)ext->private_data;
  TRACE("Close called\n");
  call_hook(&shim->close_hook);
  return 0;
}

static int init_callback(snd_pcm_extplug_t *ext)
{
  snd_pcm_shim_t *shim = (snd_pcm_shim_t *)ext->private_data;
  TRACE("Init called\n");
  call_hook(&shim->open_hook);
  return 0;
}

static const snd_pcm_extplug_callback_t shim_callbacks = {
    .init = init_callback,
    .close = close_callback,
    .transfer = transfer_callback};

SND_PCM_PLUGIN_DEFINE_FUNC(shim)
{
  snd_config_t *slave = NULL;
  snd_config_iterator_t i, next;
  snd_pcm_shim_t *shim;
  const char *open_hook_path = NULL, *close_hook_path = NULL;
  bool open_hook_blocking, close_hook_blocking;
  int err = 0;

  snd_config_for_each(i, next, conf)
  {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;

    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
      continue;
    if (strcmp(id, "slave") == 0)
    {
      slave = n;
      continue;
    }
    if (strcmp(id, "open_hook") == 0)
    {
      snd_config_iterator_t i, next;
      snd_config_for_each(i, next, n)
      {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;

        if (snd_config_get_id(n, &id) < 0)
          continue;
        if (strcmp(id, "blocking") == 0)
        {
          open_hook_blocking = snd_config_get_bool(n);
          continue;
        }
        if (strcmp(id, "path") == 0)
        {
          if (snd_config_get_string(n, &open_hook_path) < 0)
          {
            SNDERR("Could not parse open_hook.path");
            return -EINVAL;
          }
          continue;
        }
        SNDERR("Unknown field open_hook.%s", id);
        return -EINVAL;
      }
      continue;
    }
    if (strcmp(id, "close_hook") == 0)
    {
      snd_config_iterator_t i, next;
      snd_config_for_each(i, next, n)
      {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;

        if (snd_config_get_id(n, &id) < 0)
          continue;
        if (strcmp(id, "blocking") == 0)
        {
          close_hook_blocking = snd_config_get_bool(n);
          continue;
        }
        if (strcmp(id, "path") == 0)
        {
          if (snd_config_get_string(n, &close_hook_path) < 0)
          {
            SNDERR("Could not parse close_hook.path");
            return -EINVAL;
          }
          continue;
        }
        SNDERR("Unknown field close_hook.%s", id);
        return -EINVAL;
      }
      continue;
    }
    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }
  if (!slave)
  {
    SNDERR("slave is not defined");
    return -EINVAL;
  }

  shim = calloc(1, sizeof(*shim));
  if (!shim)
  {
    SNDERR("Could not allocate memory");
    return -ENOMEM;
  }

  shim->open_hook.blocking = open_hook_blocking;
  shim->open_hook.path = open_hook_path != NULL ? strdup(open_hook_path) : "";
  shim->close_hook.blocking = close_hook_blocking;
  shim->close_hook.path = close_hook_path != NULL ? strdup(close_hook_path) : "";

  shim->ext.version = SND_PCM_EXTPLUG_VERSION;
  shim->ext.name = "Shim Hooks Plugin";
  shim->ext.callback = &shim_callbacks;
  shim->ext.private_data = shim;

  call_hook(&shim->open_hook);
  TRACE("Create plugin\n");
  err = snd_pcm_extplug_create(&shim->ext, name, root, slave, stream, mode);
  if (err < 0)
  {
    TRACE("Error creating plugin %d\n", err);
    free(shim);
    return err;
  }

  snd_pcm_extplug_params_reset(&shim->ext);
  // we don't do format conversion
  snd_pcm_extplug_set_param_link(&shim->ext, SND_PCM_EXTPLUG_HW_FORMAT, 1);
  snd_pcm_extplug_set_param_link(&shim->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 1);

  *pcmp = shim->ext.pcm;

  return err;
}
SND_PCM_PLUGIN_SYMBOL(shim);