#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

// state[0] 已由适配层保存 prefetch 模式；KV 状态只使用后续槽位。
#define KV_PHASE_SLOT 1
#define KV_FD_SLOT 2
#define KV_NEXT_WRITE_SLOT 3
#define KV_NEXT_READ_SLOT 4

enum kv_phase {
  KV_CLOSED = 0,
  KV_INITIALIZED,
  KV_WRITING,
  KV_STORED,
  KV_READING,
};

static int
valid_session(struct ai_session *session)
{
  return session != 0 && session->worker >= 0 &&
         session->worker < AI_MAX_WORKERS && session->requests > 0 &&
         session->requests <= AI_MAX_REQUESTS;
}

static int
write_exact(int fd, const void *buffer, int length)
{
  const char *p = buffer;
  int n;

  while(length > 0) {
    n = write(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static int
read_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

int
ai_student_kv_begin(struct ai_session *session)
{
  if(!valid_session(session) || session->state[KV_PHASE_SLOT] != KV_CLOSED)
    return -1;

  session->state[KV_PHASE_SLOT] = KV_INITIALIZED;
  session->state[KV_FD_SLOT] = -1;
  session->state[KV_NEXT_WRITE_SLOT] = 0;
  session->state[KV_NEXT_READ_SLOT] = 0;
  return 0;
}

int
ai_student_kv_begin_write(struct ai_session *session)
{
  char name[AI_STREAM_NAME_LEN];
  int fd;

  if(!valid_session(session) ||
     session->state[KV_PHASE_SLOT] != KV_INITIALIZED ||
     session->state[KV_FD_SLOT] != -1)
    return -1;

  // 每个 worker 只有一个顺序文件。先删除上次异常退出可能留下的旧文件，
  // 保证第 0 条记录一定从一个全新的文件起点开始。
  ai_student_kv_name(name, session->worker);
  unlink(name);
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    return -1;

  session->state[KV_FD_SLOT] = fd;
  session->state[KV_NEXT_WRITE_SLOT] = 0;
  session->state[KV_PHASE_SLOT] = KV_WRITING;
  return 0;
}

int
ai_student_kv_store(struct ai_session *session, int request,
                    struct kv_entry *kv, struct ai_io *io)
{
  int fd;

  // 所有编号和阶段检查都在 write() 前完成。非法调用因此不会推进文件偏移、
  // 逻辑序号或 I/O 统计，拒绝后当前合法 request 仍可以继续提交。
  if(!valid_session(session) || kv == 0 || io == 0 ||
     session->state[KV_PHASE_SLOT] != KV_WRITING || request < 0 ||
     request >= session->requests ||
     request != session->state[KV_NEXT_WRITE_SLOT])
    return -1;
  fd = session->state[KV_FD_SLOT];
  if(fd < 0)
    return -1;

  if(write_exact(fd, kv, AI_KV_FILE_BYTES) < 0)
    return -1;

  // 完整的 2048 字节记录写完后，文件内容、统计和下一编号一起提交。
  io->kv_write_bytes += AI_KV_FILE_BYTES;
  session->state[KV_NEXT_WRITE_SLOT]++;
  return 0;
}

int
ai_student_kv_end_write(struct ai_session *session)
{
  int fd;

  if(!valid_session(session) ||
     session->state[KV_PHASE_SLOT] != KV_WRITING ||
     session->state[KV_NEXT_WRITE_SLOT] != session->requests)
    return -1;
  fd = session->state[KV_FD_SLOT];
  if(fd < 0)
    return -1;

  close(fd);
  session->state[KV_FD_SLOT] = -1;
  session->state[KV_PHASE_SLOT] = KV_STORED;
  return 0;
}

int
ai_student_kv_begin_read(struct ai_session *session)
{
  char name[AI_STREAM_NAME_LEN];
  int fd;

  if(!valid_session(session) ||
     session->state[KV_PHASE_SLOT] != KV_STORED ||
     session->state[KV_FD_SLOT] != -1)
    return -1;

  ai_student_kv_name(name, session->worker);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;

  session->state[KV_FD_SLOT] = fd;
  session->state[KV_NEXT_READ_SLOT] = 0;
  session->state[KV_PHASE_SLOT] = KV_READING;
  return 0;
}

int
ai_student_kv_restore(struct ai_session *session, int request,
                      struct kv_entry *kv, struct ai_io *io)
{
  int fd;

  // 与 store 对称：非法编号必须在 read() 前拒绝，目标页也就保持原样。
  if(!valid_session(session) || kv == 0 || io == 0 ||
     session->state[KV_PHASE_SLOT] != KV_READING || request < 0 ||
     request >= session->requests ||
     request != session->state[KV_NEXT_READ_SLOT])
    return -1;
  fd = session->state[KV_FD_SLOT];
  if(fd < 0)
    return -1;

  if(read_exact(fd, kv, AI_KV_FILE_BYTES) < 0)
    return -1;

  io->kv_read_bytes += AI_KV_FILE_BYTES;
  session->state[KV_NEXT_READ_SLOT]++;
  return 0;
}

void
ai_student_kv_end(struct ai_session *session)
{
  char name[AI_STREAM_NAME_LEN];
  int phase;

  if(session == 0)
    return;
  phase = session->state[KV_PHASE_SLOT];
  // 零初始化或已经清理过的 session 中 fd 槽可能为 0。只有确实进入过
  // KV 状态机后才拥有该 fd，避免重复 end() 误关标准输入或复用后的 fd。
  if(phase != KV_CLOSED && session->state[KV_FD_SLOT] >= 0) {
    close(session->state[KV_FD_SLOT]);
  }
  session->state[KV_FD_SLOT] = -1;
  if(phase != KV_CLOSED && session->worker >= 0 &&
     session->worker < AI_MAX_WORKERS) {
    ai_student_kv_name(name, session->worker);
    unlink(name);
  }

  session->state[KV_PHASE_SLOT] = KV_CLOSED;
  session->state[KV_NEXT_WRITE_SLOT] = 0;
  session->state[KV_NEXT_READ_SLOT] = 0;
}
