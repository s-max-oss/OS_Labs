#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

// 每个 worker 独占一份缓存。有效位必须与数据分开保存，因为分片的首字节
// 完全可能为 0，不能拿数据内容本身判断条目是否已经加载。
struct model_cache {
  uchar valid[AI_TOTAL_SHARDS];
  uchar data[AI_TOTAL_SHARDS][AI_SHARD_BYTES];
};

static int
read_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // read() 可能短读；只有循环读满 length 字节才算一次成功的文件读取。
  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static int
cache_index(uint family, uint shard)
{
  if(family == AI_MODEL_FAMILY) {
    if(shard >= AI_MODEL_SHARDS)
      return -1;
    return shard;
  }
  if(family == AI_EMBED_FAMILY) {
    if(shard >= AI_EMBED_SHARDS)
      return -1;
    return AI_MODEL_SHARDS + shard;
  }
  return -1;
}

int
ai_student_model_begin(struct ai_session *session)
{
  struct model_cache *cache;

  if(session == 0 || session->memory != 0)
    return -1;
  cache = malloc(sizeof(*cache));
  if(cache == 0)
    return -1;

  // 尚未有效的 data 不会被读取，只需清空较小的有效位数组。
  memset(cache->valid, 0, sizeof(cache->valid));
  session->memory = cache;
  return 0;
}

int
ai_student_model_load(struct ai_session *session, uint family, uint shard,
                      char *block, struct ai_io *io)
{
  struct model_cache *cache;
  char name[AI_SHARD_NAME_LEN];
  int index;
  int offset;
  int fd;

  if(session == 0 || block == 0 || io == 0 || session->memory == 0)
    return -1;
  index = cache_index(family, shard);
  if(index < 0)
    return -1;
  cache = session->memory;

  if(!cache->valid[index]) {
    ai_shard_name(name, family, shard);
    fd = open(name, O_RDONLY);
    if(fd < 0)
      return -1;
    if(read_exact(fd, cache->data[index], AI_SHARD_BYTES) < 0) {
      close(fd);
      return -1;
    }
    close(fd);

    // 文件读取和逐字节校验全部成功后才发布有效位和 I/O 统计。失败的
    // 半成品条目仍保持无效，下一次调用可以重新读取。
    for(offset = 0; offset < AI_SHARD_BYTES; offset++)
      if(cache->data[index][offset] != ai_byte(family, shard, offset))
        return -1;
    cache->valid[index] = 1;
    if(family == AI_MODEL_FAMILY)
      io->model_read_bytes += AI_SHARD_BYTES;
    else
      io->embed_read_bytes += AI_SHARD_BYTES;
  }

  // 命中时仍要向调用者交付完整分片，但这只是内存复制，不计文件 I/O。
  memmove(block, cache->data[index], AI_SHARD_BYTES);
  return 0;
}

void
ai_student_model_end(struct ai_session *session)
{
  if(session == 0 || session->memory == 0)
    return;
  free(session->memory);
  session->memory = 0;
}
