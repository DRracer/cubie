// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/* VDPAU object handles: small integers indexing a table of pointers. */
#include <pthread.h>
#include <stddef.h>

#include "vdpau_private.h"

#define MAX_HANDLES 1024

static void *handles[MAX_HANDLES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_create(void *data)
{
	uint32_t h = VDP_INVALID_HANDLE;

	pthread_mutex_lock(&lock);
	for (uint32_t i = 1; i < MAX_HANDLES; i++) {
		if (!handles[i]) {
			handles[i] = data;
			h = i;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return h;
}

void *handle_get(uint32_t handle)
{
	void *data = NULL;

	pthread_mutex_lock(&lock);
	if (handle < MAX_HANDLES)
		data = handles[handle];
	pthread_mutex_unlock(&lock);
	return data;
}

void handle_destroy(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	if (handle < MAX_HANDLES)
		handles[handle] = NULL;
	pthread_mutex_unlock(&lock);
}
