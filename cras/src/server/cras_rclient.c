/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <stdlib.h>
#include <syslog.h>

#include "audio_thread.h"
#include "cras_config.h"
#include "cras_dsp.h"
#include "cras_iodev.h"
#include "cras_iodev_list.h"
#include "cras_messages.h"
#include "cras_rclient.h"
#include "cras_rstream.h"
#include "cras_system_state.h"
#include "cras_types.h"
#include "cras_util.h"
#include "utlist.h"

/* An attached client.  This has a list of audio connections and a file
 * descriptor for communication with the client that isn't time critical. */
struct cras_rclient {
	size_t id;
	int fd; /* Connection for client communication. */
	struct cras_rstream *streams;
};

/* Handles a message from the client to connect a new stream */
static int handle_client_stream_connect(struct cras_rclient *client,
					const struct cras_connect_message *msg,
					int aud_fd)
{
	struct cras_rstream *stream;
	struct cras_client_stream_connected reply;
	struct cras_audio_format remote_fmt;
	struct audio_thread *thread;
	struct cras_iodev *dev = NULL;
	int rc;

	unpack_cras_audio_format(&remote_fmt, &msg->format);

	/* check the aud_fd is valid. */
	if (aud_fd < 0) {
		syslog(LOG_ERR, "Invalid fd in stream connect.\n");
		rc = -EINVAL;
		goto reply_err;
	}
	/* When full, getting an error is preferable to blocking. */
	cras_make_fd_nonblocking(aud_fd);

	/* Create the stream with the modified parameters. */
	rc = cras_rstream_create(msg->stream_id,
				 msg->stream_type,
				 msg->direction,
				 msg->flags,
				 &remote_fmt,
				 msg->buffer_frames,
				 msg->cb_threshold,
				 client,
				 &stream);
	if (rc < 0) {
		syslog(LOG_ERR, "Failed to create rstream.\n");
		goto reply_err;
	}

	cras_rstream_set_audio_fd(stream, aud_fd);

	/* Now can pass the stream to the thread. */
	thread = cras_iodev_list_get_audio_thread();

	DL_APPEND(client->streams, stream);

	/* Check the target device is valid for pinned streams. */
	if (msg->dev_idx != NO_DEVICE) {
		stream->is_pinned = 1;
		stream->pinned_dev_idx = msg->dev_idx;

		dev = cras_iodev_list_find_dev(msg->dev_idx);
		if (!dev) {
			rc = -EINVAL;
			goto destroy_stream_and_reply_err;
		}
	}

	rc = audio_thread_add_stream(thread, stream, dev);
	if (rc < 0) {
		syslog(LOG_ERR, "Attach stream failed.\n");
		DL_DELETE(client->streams, stream);
		goto destroy_stream_and_reply_err;
	}

	/* Tell client about the stream setup. */
	syslog(LOG_DEBUG, "Send connected for stream %x\n", msg->stream_id);
	cras_fill_client_stream_connected(
			&reply,
			0, /* No error. */
			msg->stream_id,
			&remote_fmt,
			cras_rstream_input_shm_key(stream),
			cras_rstream_output_shm_key(stream),
			cras_rstream_get_total_shm_size(stream));
	rc = cras_rclient_send_message(client, &reply.header);
	if (rc < 0) {
		syslog(LOG_ERR, "Failed to send connected messaged\n");
		audio_thread_disconnect_stream(thread, stream);
		DL_DELETE(client->streams, stream);
		goto reply_err;
	}

	cras_system_state_stream_added(stream->direction);

	return 0;

destroy_stream_and_reply_err:
	cras_rstream_destroy(stream);
reply_err:
	/* Send the error code to the client. */
	cras_fill_client_stream_connected(&reply, rc, msg->stream_id,
					  &remote_fmt, 0, 0, 0);
	cras_rclient_send_message(client, &reply.header);

	if (aud_fd >= 0)
		close(aud_fd);

	return rc;
}

/* Removes the stream from the current device it is being played/captured on and
 * from the list of streams for the client. */
static int disconnect_client_stream(struct cras_rclient *client,
				    struct cras_rstream *stream)
{
	enum CRAS_STREAM_DIRECTION direction = stream->direction;
	struct audio_thread *thread = cras_iodev_list_get_audio_thread();
	int aud_fd = cras_rstream_get_audio_fd(stream);

	DL_DELETE(client->streams, stream);
	audio_thread_disconnect_stream(thread, stream);

	close(aud_fd);
	cras_system_state_stream_removed(direction);

	return 0;
}

/* Handles messages from the client requesting that a stream be removed from the
 * server. */
static int handle_client_stream_disconnect(
		struct cras_rclient *client,
		const struct cras_disconnect_stream_message *msg)
{
	struct cras_rstream *to_disconnect;

	DL_SEARCH_SCALAR(client->streams, to_disconnect, stream_id,
			 msg->stream_id);
	if (!to_disconnect)
		return -EINVAL;

	return disconnect_client_stream(client, to_disconnect);
}

/* Handles dumping audio thread debug info back to the client. */
static void dump_audio_thread_info(struct cras_rclient *client)
{
	struct cras_client_audio_debug_info_ready msg;
	struct cras_server_state *state;

	cras_fill_client_audio_debug_info_ready(&msg);
	state = cras_system_state_get_no_lock();
	audio_thread_dump_thread_info(cras_iodev_list_get_audio_thread(),
				      &state->audio_debug_info);
	cras_rclient_send_message(client, &msg.header);
}

/*
 * Exported Functions.
 */

/* Creates a client structure and sends a message back informing the client that
 * the conneciton has succeeded. */
struct cras_rclient *cras_rclient_create(int fd, size_t id)
{
	struct cras_rclient *client;
	struct cras_client_connected msg;

	client = calloc(1, sizeof(struct cras_rclient));
	if (!client)
		return NULL;

	client->fd = fd;
	client->id = id;

	cras_fill_client_connected(&msg, client->id, cras_sys_state_shm_key());
	cras_rclient_send_message(client, &msg.header);

	return client;
}

/* Removes all streams that the client owns and destroys it. */
void cras_rclient_destroy(struct cras_rclient *client)
{
	struct cras_rstream *stream;
	DL_FOREACH(client->streams, stream) {
		disconnect_client_stream(client, stream);
	}
	free(client);
}

/* Entry point for handling a message from the client.  Called from the main
 * server context. */
int cras_rclient_message_from_client(struct cras_rclient *client,
				     const struct cras_server_message *msg,
				     int fd) {
	assert(client && msg);

	/* Most messages should not have a file descriptor. */
	switch (msg->id) {
	case CRAS_SERVER_CONNECT_STREAM:
		break;
	default:
		if (fd != -1) {
			syslog(LOG_ERR,
			       "Message %d should not have fd attached.",
			       msg->id);
			close(fd);
			return -1;
		}
		break;
	}

	switch (msg->id) {
	case CRAS_SERVER_CONNECT_STREAM:
		handle_client_stream_connect(client,
			(const struct cras_connect_message *)msg, fd);
		break;
	case CRAS_SERVER_DISCONNECT_STREAM:
		handle_client_stream_disconnect(client,
			(const struct cras_disconnect_stream_message *)msg);
		break;
	case CRAS_SERVER_SET_SYSTEM_VOLUME:
		cras_system_set_volume(
			((const struct cras_set_system_volume *)msg)->volume);
		break;
	case CRAS_SERVER_SET_SYSTEM_MUTE:
		cras_system_set_mute(
			((const struct cras_set_system_mute *)msg)->mute);
		break;
	case CRAS_SERVER_SET_USER_MUTE:
		cras_system_set_user_mute(
			((const struct cras_set_system_mute *)msg)->mute);
		break;
	case CRAS_SERVER_SET_SYSTEM_MUTE_LOCKED:
		cras_system_set_mute_locked(
			((const struct cras_set_system_mute *)msg)->mute);
		break;
	case CRAS_SERVER_SET_SYSTEM_CAPTURE_GAIN: {
		const struct cras_set_system_capture_gain *m =
			(const struct cras_set_system_capture_gain *)msg;
		cras_system_set_capture_gain(m->gain);
		break;
	}
	case CRAS_SERVER_SET_SYSTEM_CAPTURE_MUTE:
		cras_system_set_capture_mute(
			((const struct cras_set_system_mute *)msg)->mute);
		break;
	case CRAS_SERVER_SET_SYSTEM_CAPTURE_MUTE_LOCKED:
		cras_system_set_capture_mute_locked(
			((const struct cras_set_system_mute *)msg)->mute);
		break;
	case CRAS_SERVER_SET_NODE_ATTR: {
		const struct cras_set_node_attr *m =
			(const struct cras_set_node_attr *)msg;
		cras_iodev_list_set_node_attr(m->node_id, m->attr, m->value);
		break;
	}
	case CRAS_SERVER_SELECT_NODE: {
		const struct cras_select_node *m =
			(const struct cras_select_node *)msg;
		cras_iodev_list_select_node(m->direction, m->node_id);
		break;
	}
	case CRAS_SERVER_ADD_ACTIVE_NODE: {
		const struct cras_add_active_node *m =
			(const struct cras_add_active_node *)msg;
		cras_iodev_list_add_active_node(m->direction, m->node_id);
		break;
	}
	case CRAS_SERVER_RM_ACTIVE_NODE: {
		const struct cras_rm_active_node *m =
			(const struct cras_rm_active_node *)msg;
		cras_iodev_list_rm_active_node(m->direction, m->node_id);
		break;
	}
	case CRAS_SERVER_RELOAD_DSP:
		cras_dsp_reload_ini();
		break;
	case CRAS_SERVER_DUMP_DSP_INFO:
		cras_dsp_dump_info();
		break;
	case CRAS_SERVER_DUMP_AUDIO_THREAD:
		dump_audio_thread_info(client);
		break;
	case CRAS_SERVER_ADD_TEST_DEV: {
		const struct cras_add_test_dev *m =
			(const struct cras_add_test_dev *)msg;
		cras_iodev_list_add_test_dev(m->type);
		break;
	}
	case CRAS_SERVER_TEST_DEV_COMMAND: {
		const struct cras_test_dev_command *m =
			(const struct cras_test_dev_command *)msg;
		cras_iodev_list_test_dev_command(m->iodev_idx, m->command,
						 m->data_len, m->data);
		break;
	}
	default:
		break;
	}

	return 0;
}

/* Sends a message to the client. */
int cras_rclient_send_message(const struct cras_rclient *client,
			      const struct cras_client_message *msg)
{
	return write(client->fd, msg, msg->length);
}

