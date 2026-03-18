#pragma once

#include <gui/view.h>

typedef struct CompromisedView CompromisedView;
typedef void (*CompromisedViewDoneCallback)(void* context);

CompromisedView* compromised_view_alloc(void);
void compromised_view_free(CompromisedView* compromised_view);
View* compromised_view_get_view(CompromisedView* compromised_view);
void compromised_view_set_done_callback(
    CompromisedView* compromised_view,
    CompromisedViewDoneCallback callback,
    void* context);
void compromised_view_start(CompromisedView* compromised_view);
