#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

// 这些槽位与 ai_student_kv.c 的布局一致。state[0] 由适配层保存模式，
// state[5] 表示 session->memory 已从模型缓存切换为 prefetch_state。
#define KV_PHASE_SLOT 1
#define KV_FD_SLOT 2
#define KV_READING 4
#define PREFETCH_ACTIVE_SLOT 5

#define NO_REQUEST -1
#define STOP_REQUEST -1

struct prefetch_state {
  int child_pid;
  int request_write;
  int data_read;
  int pending_request;
  int ready_request;
  int next_request;
  int failed;
  // 固定单槽窗口限制了内存用量，也防止尚未消费的数据被下一条覆盖。
  struct kv_entry window[AI_KV_TOKENS];
};

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

static void
helper_loop(struct ai_session *session, struct prefetch_state *state,
            int request_read, int data_write)
{
  char name[AI_STREAM_NAME_LEN];
  int expected = 0;
  int request;
  int fd = -1;
  int status = 1;

  // 辅助进程重新 open，拥有独立文件偏移；它不会与父进程交替推进同一个
  // struct file。请求仍必须按 0、1、2……顺序到达。
  ai_student_kv_name(name, session->worker);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    goto done;

  for(;;) {
    if(read_exact(request_read, &request, sizeof(request)) < 0)
      goto done;
    if(request == STOP_REQUEST) {
      status = 0;
      goto done;
    }
    if(request != expected || request < 0 || request >= session->requests)
      goto done;
    if(read_exact(fd, state->window, AI_KV_FILE_BYTES) < 0)
      goto done;
    // 回包先带 request 编号，父进程据此拒绝迟到或错序的数据。
    if(write_exact(data_write, &request, sizeof(request)) < 0 ||
       write_exact(data_write, state->window, AI_KV_FILE_BYTES) < 0)
      goto done;
    expected++;
  }

done:
  if(fd >= 0)
    close(fd);
  close(request_read);
  close(data_write);
  exit(status);
}

static struct prefetch_state *
get_state(struct ai_session *session)
{
  if(session == 0 || !session->state[PREFETCH_ACTIVE_SLOT] ||
     session->memory == 0)
    return 0;
  return session->memory;
}

int
ai_student_prefetch_begin(struct ai_session *session)
{
  struct prefetch_state *state;
  int request_pipe[2];
  int data_pipe[2];
  int pid;

  if(session == 0 || !session->state[0] ||
     session->state[PREFETCH_ACTIVE_SLOT] ||
     session->state[KV_PHASE_SLOT] != KV_READING ||
     session->state[KV_FD_SLOT] < 0 || session->memory == 0)
    return -1;

  state = malloc(sizeof(*state));
  if(state == 0)
    return -1;
  state->child_pid = -1;
  state->request_write = -1;
  state->data_read = -1;
  state->pending_request = NO_REQUEST;
  state->ready_request = NO_REQUEST;
  state->next_request = 0;
  state->failed = 0;

  if(pipe(request_pipe) < 0) {
    free(state);
    return -1;
  }
  if(pipe(data_pipe) < 0) {
    close(request_pipe[0]);
    close(request_pipe[1]);
    free(state);
    return -1;
  }

  pid = fork();
  if(pid < 0) {
    close(request_pipe[0]);
    close(request_pipe[1]);
    close(data_pipe[0]);
    close(data_pipe[1]);
    free(state);
    return -1;
  }
  if(pid == 0) {
    close(request_pipe[1]);
    close(data_pipe[0]);
    // 关闭 fork 继承的父进程恢复 fd；helper_loop 会自行打开只读文件。
    close(session->state[KV_FD_SLOT]);
    helper_loop(session, state, request_pipe[0], data_pipe[1]);
  }

  // 父进程只保留请求写端和数据读端。
  close(request_pipe[0]);
  close(data_pipe[1]);
  close(session->state[KV_FD_SLOT]);
  session->state[KV_FD_SLOT] = -1;

  // prefill 已全部结束，模型缓存不会再使用。释放后复用 memory 字段保存
  // 固定预取窗口；prefetch_end 会在 model_end 之前将它清空。
  free(session->memory);
  state->child_pid = pid;
  state->request_write = request_pipe[1];
  state->data_read = data_pipe[0];
  session->memory = state;
  session->state[PREFETCH_ACTIVE_SLOT] = 1;
  return 0;
}

int
ai_student_prefetch_next(struct ai_session *session, int request)
{
  struct prefetch_state *state = get_state(session);

  if(state == 0 || state->failed || request < 0 ||
     request >= session->requests || request != state->next_request ||
     state->pending_request != NO_REQUEST ||
     state->ready_request != NO_REQUEST)
    return -1;

  // 这里只发送很小的请求编号并立即返回；实际文件读取由 helper 完成。
  if(write_exact(state->request_write, &request, sizeof(request)) < 0) {
    state->failed = 1;
    return -1;
  }
  state->pending_request = request;
  return 0;
}

int
ai_student_prefetch_wait(struct ai_session *session, int request)
{
  struct prefetch_state *state = get_state(session);
  int completed;

  if(state == 0 || state->failed ||
     state->pending_request != request ||
     state->ready_request != NO_REQUEST)
    return -1;

  if(read_exact(state->data_read, &completed, sizeof(completed)) < 0 ||
     completed != request ||
     read_exact(state->data_read, state->window, AI_KV_FILE_BYTES) < 0) {
    state->failed = 1;
    return -1;
  }

  // 只有编号和 2048 字节记录均完整到达后，窗口才从 PENDING 变为 READY。
  state->pending_request = NO_REQUEST;
  state->ready_request = request;
  return 0;
}

int
ai_student_prefetch_restore(struct ai_session *session, int request,
                            struct kv_entry *kv, struct ai_io *io)
{
  struct prefetch_state *state = get_state(session);

  if(state == 0 || state->failed || kv == 0 || io == 0 ||
     session->state[KV_PHASE_SLOT] != KV_READING ||
     request != state->next_request || state->ready_request != request)
    return -1;

  memmove(kv, state->window, AI_KV_FILE_BYTES);
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  state->ready_request = NO_REQUEST;
  state->next_request++;
  return 0;
}

void
ai_student_prefetch_end(struct ai_session *session)
{
  struct prefetch_state *state = get_state(session);
  int stop = STOP_REQUEST;
  int abnormal;

  if(state == 0)
    return;

  abnormal = state->failed || state->pending_request != NO_REQUEST ||
             state->ready_request != NO_REQUEST;
  if(!abnormal && state->request_write >= 0 &&
     write_exact(state->request_write, &stop, sizeof(stop)) < 0)
    abnormal = 1;

  // 错误时 helper 可能阻塞在 512 字节 pipe 上；kill 会使阻塞读写返回，
  // 随后的 close + wait 保证既不死锁，也不留下僵尸进程。
  if(abnormal && state->child_pid > 0)
    kill(state->child_pid);
  if(state->request_write >= 0)
    close(state->request_write);
  if(state->data_read >= 0)
    close(state->data_read);
  if(state->child_pid > 0)
    wait(0);

  free(state);
  session->memory = 0;
  session->state[PREFETCH_ACTIVE_SLOT] = 0;
}
