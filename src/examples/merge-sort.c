#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define MAX_NUMBERS 10
#define MAX_FILES 3

typedef struct {
    char *filename;
    int numbers[MAX_NUMBERS];
    int count;
} FileData;

lock_t lock;

static int compare_ints (const void *a_, const void *b_) {
  const int *a = a_;
  const int *b = b_;

  return *a < *b ? -1 : *a > *b;
}

void* sort_file(void *arg) {
  FileData *data = (FileData*) arg;
  int fd = open(data->filename);
  if (fd == -1) {
    printf("thread [%d]: Error opening file %s\n", get_tid(), data->filename);
    pthread_exit();
  }
  char buffer[30];
  size_t bytesRead = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (bytesRead == 0) {
    printf("thread [%d]: Error reading file %s\n", get_tid(), data->filename);
    pthread_exit();
  }
  buffer[bytesRead] = '\0';
  int count = 0;
  char *token, *save_ptr;
  for (token = strtok_r (buffer, " \n", &save_ptr);
       token != NULL && count < MAX_NUMBERS;
       token = strtok_r (NULL, " \n", &save_ptr)) {
    data->numbers[count++] = atoi(token);
  }
  data->count = count;
  lock_acquire(&lock);
  printf("\n----data of thread [%d]----\n", get_tid());
  for(int i=0;i<count;i++){
    printf("%d ", data->numbers[i]);
  }
  printf("\n");
  lock_release(&lock);
  qsort(data->numbers, data->count, sizeof(int), compare_ints);
  pthread_exit();
}

void merge_sorted_files(FileData files[], int file_count, const char *output_filename) {
  bool success = create(output_filename, 0);
  if (!success) {
    printf("thread [%d]: Error creating file %s\n", get_tid(), output_filename);
    return;
  }
  int output_fd = open(output_filename);
  if (output_fd == -1) {
    printf("thread [%d]: Error opening file %s\n", get_tid(), output_filename);
    return;
  }
  int indices[MAX_FILES] = {0};
  printf("sorted data: ");
  while (1) {
    int min_val = 2e9;
    int min_file = -1;
    for (int i = 0; i < file_count; ++i) {
      if (indices[i] < files[i].count && files[i].numbers[indices[i]] < min_val) {
        min_val = files[i].numbers[indices[i]];
        min_file = i;
      }
    }
    if (min_file == -1) {
      break;
    }
    printf("%d ", min_val);
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%d\n", min_val);
    write(output_fd, buffer, len);
    indices[min_file]++;
  }
  printf("\n\n");
  close(output_fd);
}

int main(void) {
  lock_init(&lock);
  const char *output_filename = "data-sorted";
  int file_count = 2;
  FileData files[MAX_FILES];
  files[0].filename = "data-0";
  files[1].filename = "data-1";
  tid_t threads[MAX_FILES];
  for (int i = 0; i < file_count; ++i) {
    threads[i] = pthread_create(sort_file, &files[i]);
  }
  for (int i = 0; i < file_count; ++i) {
    pthread_join(threads[i]);
  }
  merge_sorted_files(files, file_count, output_filename);
  return 0;
}