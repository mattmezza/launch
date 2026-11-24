#ifndef LAUNCH_FILES_H
#define LAUNCH_FILES_H

#include <glib.h>
#include <stdbool.h>

typedef struct FileSearch FileSearch;

typedef struct {
    char *path;
    bool directory;
} FileMatch;

typedef struct {
    int request;
    GPtrArray *matches;
    bool pending;
} FileResults;

FileSearch *files_create(char **roots, char **exclude, bool hidden, int cache_seconds, int limit);
void files_close(FileSearch *);
int files_fd(const FileSearch *);
int files_request(FileSearch *, const char *query);
FileResults *files_receive(FileSearch *);
void files_results_free(FileResults *);

#endif
