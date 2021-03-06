/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <alsa/asoundlib.h>
#include <linux/input.h>
#include <syslog.h>

#include "cras_alsa_jack.h"
#include "cras_alsa_mixer.h"
#include "cras_alsa_ucm.h"
#include "cras_system_state.h"
#include "cras_gpio_jack.h"
#include "cras_tm.h"
#include "cras_util.h"
#include "edid_utils.h"
#include "utlist.h"

static const unsigned int DISPLAY_INFO_RETRY_DELAY_MS = 200;
static const unsigned int DISPLAY_INFO_MAX_RETRIES = 10;

/* Constants used to retrieve monitor name from ELD buffer. */
static const unsigned int ELD_MNL_MASK = 31;
static const unsigned int ELD_MNL_OFFSET = 4;
static const unsigned int ELD_MONITOR_NAME_OFFSET = 20;

/* Keeps an fd that is registered with system settings.  A list of fds must be
 * kept so that they can be removed when the jack list is destroyed. */
struct jack_poll_fd {
	int fd;
	struct jack_poll_fd *prev, *next;
};

/* cras_gpio_jack:  Describes headphone & microphone jack connected to GPIO
 *
 *   On Arm-based systems, the headphone & microphone jacks are
 *   connected to GPIOs which are plumbed through the /dev/input/event
 *   system.  For these jacks, the software is written to open the
 *   corresponding /dev/input/event file and monitor it for 'insert' &
 *   'remove' activity.
 *
 *   fd           : File descriptor corresponding to the /dev/input/event file.
 *
 *   switch_event : Indicates the type of the /dev/input/event file.
 *                  Either SW_HEADPHONE_INSERT, or SW_MICROPHONE_INSERT.
 *
 *   current_state: 0 -> device not plugged in
 *                  1 -> device plugged in
 *   device_name  : Device name extracted from /dev/input/event[0..9]+.
 *                  Allocated on heap; must free.
 */
struct cras_gpio_jack {
	int fd;
	unsigned switch_event;
	unsigned current_state;
	char *device_name;
};

/* Represents a single alsa Jack, e.g. "Headphone Jack" or "Mic Jack".
 *    is_gpio: 1 -> gpio switch (union field: gpio)
 *             0 -> Alsa 'jack' (union field: elem)
 *    elem - alsa hcontrol element for this jack, when is_gpio == 0.
 *    gpio - description of gpio-based jack, when is_gpio != 0.
 *    eld_control - mixer control for ELD info buffer.
 *    jack_list - list of jacks this belongs to.
 *    mixer_output - mixer output control used to control audio to this jack.
 *        This will be null for input jacks.
 *    ucm_device - Name of the ucm device if found, otherwise, NULL.
 *    edid_file - File to read the EDID from (if available, HDMI only).
 *    display_info_timer - Timer used to poll display info for HDMI jacks.
 *    display_info_retries - Number of times to retry reading display info.
 */
struct cras_alsa_jack {
	unsigned is_gpio;	/* !0 -> 'gpio' valid
				 *  0 -> 'elem' valid
				 */
	union {
		snd_hctl_elem_t *elem;
		struct cras_gpio_jack gpio;
	};

	snd_hctl_elem_t *eld_control;
	struct cras_alsa_jack_list *jack_list;
	struct cras_alsa_mixer_output *mixer_output;
	struct mixer_volume_control *mixer_input;
	char *ucm_device;
	const char *dsp_name;
	const char* override_type_name;
	const char *edid_file;
	struct cras_timer *display_info_timer;
	unsigned int display_info_retries;
	struct cras_alsa_jack *prev, *next;
};

/* Contains all Jacks for a given device.
 *    hctl - alsa hcontrol for this device.
 *    mixer - cras mixer for the card providing this device.
 *    device_index - Index ALSA uses to refer to the device.  The Y in "hw:X,Y".
 *    registered_fds - list of fds registered with system, to be removed upon
 *        destruction.
 *    change_callback - function to call when the state of a jack changes.
 *    callback_data - data to pass back to the callback.
 *    jacks - list of jacks for this device.
 */
struct cras_alsa_jack_list {
	snd_hctl_t *hctl;
	struct cras_alsa_mixer *mixer;
	snd_use_case_mgr_t *ucm;
	size_t device_index;
	struct jack_poll_fd *registered_fds;
	jack_state_change_callback *change_callback;
	void *callback_data;
	struct cras_alsa_jack *jacks;
};

/*
 * Local Helpers.
 */

#define BITS_PER_BYTE		(8)
#define BITS_PER_LONG		(sizeof(long) * BITS_PER_BYTE)
#define NBITS(x)		((((x) - 1) / BITS_PER_LONG) + 1)
#define OFF(x)			((x) % BITS_PER_LONG)
#define BIT(x)			(1UL << OFF(x))
#define LONG(x)			((x) / BITS_PER_LONG)
#define IS_BIT_SET(bit, array)	!!((array[LONG(bit)]) & (1UL << OFF(bit)))

static unsigned sys_input_get_switch_state(int fd, unsigned sw, unsigned *state)
{
	unsigned long bits[NBITS(SW_CNT)];
	const unsigned long switch_no = sw;

	memset(bits, '\0', sizeof(bits));
	/* If switch event present & supported, get current state. */
	if (gpio_switch_eviocgbit(fd, bits, sizeof(bits)) < 0)
		return -EIO;

	if (IS_BIT_SET(switch_no, bits))
		if (gpio_switch_eviocgsw(fd, bits, sizeof(bits)) >= 0) {
			*state = IS_BIT_SET(switch_no, bits);
			return 0;
		}

	return -1;
}

static inline struct cras_alsa_jack *cras_alloc_jack(int is_gpio)
{
	struct cras_alsa_jack *jack = calloc(1, sizeof(*jack));
	if (jack == NULL)
		return NULL;
	jack->is_gpio = is_gpio;
	return jack;
}

/* Gets the current plug state of the jack */
static int get_jack_current_state(struct cras_alsa_jack *jack)
{
	snd_ctl_elem_value_t *elem_value;

	if (jack->is_gpio)
		return jack->gpio.current_state;

	snd_ctl_elem_value_alloca(&elem_value);
	snd_hctl_elem_read(jack->elem, elem_value);

	return snd_ctl_elem_value_get_boolean(elem_value, 0);
}

static int check_jack_edid(struct cras_alsa_jack *jack)
{
	int fd, nread;
	uint8_t edid[EEDID_SIZE];

	/* If the jack supports EDID, check that it supports audio, clearing
	 * the plugged state if it doesn't.  If the EDID isn't ready, try
	 * again later.
	 */
	fd = open(jack->edid_file, O_RDONLY);
	if (fd < 0)
		return -1;

	nread = read(fd, edid, EEDID_SIZE);
	close(fd);

	if (nread < EDID_SIZE || !edid_valid(edid))
		return -1;

	/* Valid EDID */
	if (!edid_lpcm_support(edid, edid[EDID_EXT_FLAG]))
		jack->gpio.current_state = 0;
	return 0;
}

/* Checks the ELD control of the jack to see if the ELD buffer
 * is ready to read and report the plug status.
 */
static int check_jack_eld(struct cras_alsa_jack *jack)
{
	snd_ctl_elem_info_t *elem_info;
	snd_ctl_elem_info_alloca(&elem_info);

	/* Poll ELD control by getting the count of ELD buffer.
	 * When seeing zero buffer count, retry after a delay until
	 * it's ready or reached the max number of retries. */
	if (snd_hctl_elem_info(jack->eld_control, elem_info) != 0)
		return -1;
	if (snd_ctl_elem_info_get_count(elem_info) == 0)
		return -1;
	return 0;
}

static void display_info_delay_cb(struct cras_timer *timer, void *arg);

/* Callback function doing following things:
 * 1. Reset timer and update max number of retries.
 * 2. Check all conditions to see if it's okay or needed to
 *    report jack status directly. E.g. jack is unplugged or
 *    EDID is not ready for some reason.
 * 3. Check if max number of retries is reached and decide
 *    to set timer for next callback or report jack state.
 */
static inline void jack_state_change_cb(struct cras_alsa_jack *jack, int retry)
{
	struct cras_tm *tm = cras_system_state_get_tm();

	if (jack->display_info_timer) {
		cras_tm_cancel_timer(tm, jack->display_info_timer);
		jack->display_info_timer = NULL;
	}
	if (retry)
		jack->display_info_retries = DISPLAY_INFO_MAX_RETRIES;

	if (!get_jack_current_state(jack))
		goto report_jack_state;

	/* If there is an edid file, check it.  If it is ready continue, if we
	 * need to try again later, return here as the timer has been armed and
	 * will check again later.
	 */
	if (jack->edid_file == NULL && jack->eld_control == NULL)
		goto report_jack_state;
	if (jack->edid_file && (check_jack_edid(jack) == 0))
		goto report_jack_state;
	if (jack->eld_control && (check_jack_eld(jack) == 0))
		goto report_jack_state;

	if (--jack->display_info_retries == 0) {
		if (jack->is_gpio)
			jack->gpio.current_state = 0;
		if (jack->edid_file)
			syslog(LOG_ERR, "Timeout to read EDID from %s",
			       jack->edid_file);
		goto report_jack_state;
	}

	jack->display_info_timer = cras_tm_create_timer(tm,
						DISPLAY_INFO_RETRY_DELAY_MS,
						display_info_delay_cb, jack);
	return;

report_jack_state:
	jack->jack_list->change_callback(jack,
					 get_jack_current_state(jack),
					 jack->jack_list->callback_data);
}

/* gpio_switch_initial_state
 *
 *   Determines the initial state of a gpio-based switch.
 */
static void gpio_switch_initial_state(struct cras_alsa_jack *jack)
{
	unsigned v;
	unsigned r = sys_input_get_switch_state(jack->gpio.fd,
						jack->gpio.switch_event, &v);
	jack->gpio.current_state = r == 0 ? v : 0;
	jack_state_change_cb(jack, 1);
}

/* Check if the input event is an audio switch event. */
static inline int is_audio_switch_event(const struct input_event *ev,
					int sw_code)
{
	return (ev->type == EV_SW && ev->code == sw_code);
}

/* Timer callback to read display info after a hotplug event for an HDMI jack.
 */
static void display_info_delay_cb(struct cras_timer *timer, void *arg)
{
	struct cras_alsa_jack *jack = (struct cras_alsa_jack *)arg;

	jack->display_info_timer = NULL;
	jack_state_change_cb(jack, 0);
}

/* gpio_switch_callback
 *
 *   This callback is invoked whenever the associated /dev/input/event
 *   file has data to read.  Perform autoswitching to / from the
 *   associated device when data is available.
 */
static void gpio_switch_callback(void *arg)
{
	struct cras_alsa_jack *jack = arg;
	int i;
	int r;
	struct input_event ev[64];

	r = gpio_switch_read(jack->gpio.fd, ev,
			     ARRAY_SIZE(ev) * sizeof(struct input_event));
	if (r < 0)
		return;

	for (i = 0; i < r / sizeof(struct input_event); ++i)
		if (is_audio_switch_event(&ev[i], jack->gpio.switch_event)) {
			jack->gpio.current_state = ev[i].value;

			jack_state_change_cb(jack, 1);
		}
}

/* open_and_monitor_gpio:
 *
 *   Opens a /dev/input/event file associated with a headphone /
 *   microphone jack and watches it for activity.
 *   Returns 0 when a jack has been successfully added.
 */
static int open_and_monitor_gpio(struct cras_alsa_jack_list *jack_list,
				 enum CRAS_STREAM_DIRECTION direction,
				 const char *card_name,
				 const char *pathname,
				 unsigned switch_event)
{
	struct cras_alsa_jack *jack;
	unsigned long bits[NBITS(SW_CNT)];
	int r;

	jack = cras_alloc_jack(1);
	if (jack == NULL)
		return -ENOMEM;

	jack->gpio.fd = gpio_switch_open(pathname);
	if (jack->gpio.fd == -1) {
		free(jack);
		return -EIO;
	}

	jack->gpio.switch_event = switch_event;
	jack->jack_list = jack_list;
	jack->gpio.device_name = sys_input_get_device_name(pathname);

	if (!jack->gpio.device_name ||
	    !strstr(jack->gpio.device_name, card_name) ||
	    (gpio_switch_eviocgbit(jack->gpio.fd, bits, sizeof(bits)) < 0) ||
	    !IS_BIT_SET(switch_event, bits)) {
		close(jack->gpio.fd);
		free(jack->gpio.device_name);
		free(jack);
		return -EIO;
	}

	DL_APPEND(jack_list->jacks, jack);

	if (direction == CRAS_STREAM_OUTPUT &&
	    (strstr(jack->gpio.device_name, "Headphone") ||
	     strstr(jack->gpio.device_name, "Headset")))
		jack->mixer_output = cras_alsa_mixer_get_output_matching_name(
			jack_list->mixer,
			jack_list->device_index,
			"Headphone");
	else if (direction == CRAS_STREAM_OUTPUT &&
		 strstr(jack->gpio.device_name, "HDMI"))
		jack->mixer_output = cras_alsa_mixer_get_output_matching_name(
			jack_list->mixer,
			jack_list->device_index,
			"HDMI");

	if (jack_list->ucm)
		jack->ucm_device =
			ucm_get_dev_for_jack(jack_list->ucm,
					     jack->gpio.device_name,
					     direction);

	if (jack->ucm_device)
		jack->edid_file = ucm_get_edid_file_for_dev(jack_list->ucm,
							    jack->ucm_device);

	if (jack->ucm_device && direction == CRAS_STREAM_INPUT) {
		char *control_name;
		control_name = ucm_get_cap_control(jack->jack_list->ucm,
						   jack->ucm_device);
		if (control_name)
			jack->mixer_input =
				cras_alsa_mixer_get_input_matching_name(
					jack_list->mixer,
					control_name);
	}

        if (jack->ucm_device) {
                jack->dsp_name = ucm_get_dsp_name(
                        jack->jack_list->ucm, jack->ucm_device, direction);
        }

	sys_input_get_switch_state(jack->gpio.fd, switch_event,
				   &jack->gpio.current_state);
	r = cras_system_add_select_fd(jack->gpio.fd,
				      gpio_switch_callback, jack);
	return r;
}

static int wait_for_dev_input_access()
{
	/* Wait for /dev/input/event* files to become accessible by
	 * having group 'input'.  Setting these files to have 'rw'
	 * access to group 'input' is done through a udev rule
	 * installed by adhd into /lib/udev/rules.d.
	 *
	 * Wait for up to 2 seconds for the /dev/input/event* files to be
	 * readable by gavd.
	 *
	 * TODO(thutt): This could also be done with a udev enumerate
	 *              and then a udev monitor.
	 */
	const unsigned max_iterations = 4;
	unsigned i = 0;

	while (i < max_iterations) {
		int		   readable;
		struct timeval	   timeout;
		const char * const pathname = "/dev/input/event0";

		timeout.tv_sec	= 0;
		timeout.tv_usec = 500000;   /* 1/2 second. */
		readable = access(pathname, O_RDONLY);

		/* If the file could be opened, then the udev rule has been
		 * applied and gavd can read the event files.  If there are no
		 * event files, then we don't need to wait.
		 *
		 * If access does not become available, then headphone &
		 * microphone jack autoswitching will not function properly.
		 */
		if (readable == 0 || (readable == -1 && errno == ENOENT)) {
			/* Access allowed, or file does not exist. */
			break;
		}
		if (readable != -1 || errno != EACCES) {
			syslog(LOG_ERR, "Bad access for input devs.");
			return errno;
		}
		select(1, NULL, NULL, NULL, &timeout);
		++i;
	}

	return 0;
}

static void find_gpio_jacks(struct cras_alsa_jack_list *jack_list,
			    unsigned int card_index,
			    const char *card_name,
			    enum CRAS_STREAM_DIRECTION direction)
{
	/* GPIO switches are on Arm-based machines, and are
	 * only associated with on-board devices.
	 */
	char *devices[32];
	unsigned n_devices;
	unsigned i;
	static const int out_switches[] = {SW_HEADPHONE_INSERT,
					   SW_LINEOUT_INSERT};
	static const int in_switches[] = {SW_MICROPHONE_INSERT};

	if (wait_for_dev_input_access())
		return;

	n_devices = gpio_get_switch_names(direction, devices,
					  ARRAY_SIZE(devices));
	for (i = 0; i < n_devices; ++i) {
		int sw;
		const int *switches = out_switches;
		int num_switches = ARRAY_SIZE(out_switches);

		if (direction == CRAS_STREAM_INPUT) {
			switches = in_switches;
			num_switches = ARRAY_SIZE(in_switches);
		}

		for (sw = 0; sw < num_switches; sw++)
			open_and_monitor_gpio(jack_list, direction, card_name,
					      devices[i], switches[sw]);
		free(devices[i]);
	}
}

/* Callback from alsa when a jack control changes.  This is registered with
 * snd_hctl_elem_set_callback in find_jack_controls and run by calling
 * snd_hctl_handle_events in alsa_control_event_pending below.
 * Args:
 *    elem - The ALSA control element that has changed.
 *    mask - unused.
 */
static int hctl_jack_cb(snd_hctl_elem_t *elem, unsigned int mask)
{
	const char *name;
	snd_ctl_elem_value_t *elem_value;
	struct cras_alsa_jack *jack;

	jack = snd_hctl_elem_get_callback_private(elem);
	if (jack == NULL) {
		syslog(LOG_ERR, "Invalid jack from control event.");
		return -EINVAL;
	}

	snd_ctl_elem_value_alloca(&elem_value);
	snd_hctl_elem_read(elem, elem_value);
	name = snd_hctl_elem_get_name(elem);

	syslog(LOG_DEBUG,
	       "Jack %s %s",
	       name,
	       snd_ctl_elem_value_get_boolean(elem_value, 0) ? "plugged"
							     : "unplugged");
	jack_state_change_cb(jack, 1);
	return 0;
}

/* Handles notifications from alsa controls.  Called by main thread when a poll
 * fd provided by alsa signals there is an event available. */
static void alsa_control_event_pending(void *arg)
{
	struct cras_alsa_jack_list *jack_list;

	jack_list = (struct cras_alsa_jack_list *)arg;
	if (jack_list == NULL) {
		syslog(LOG_ERR, "Invalid jack_list from control event.");
		return;
	}

	/* handle_events will trigger the callback registered with each control
	 * that has changed. */
	snd_hctl_handle_events(jack_list->hctl);
}

/* Determines the device associated with this jack if any.  If the device cannot
 * be determined (common case), assume device 0. */
static unsigned int jack_device_index(const char *name)
{
	/* Look for the substring 'pcm=<device number>' in the element name. */
	static const char pcm_search[] = "pcm=";
	const char *substr;
	int device_index;

	substr = strstr(name, pcm_search);
	if (substr == NULL)
		return 0;
	substr += ARRAY_SIZE(pcm_search) - 1;
	if (*substr == '\0')
		return 0;
	device_index = atoi(substr);
	if (device_index < 0)
		return 0;
	return (unsigned int)device_index;
}

/* For non-gpio jack, check if it's of type hdmi/dp by
 * matching jack name. */
static int is_jack_hdmi_dp(const char *jack_name)
{
	static const char *hdmi_dp = "HDMI/DP";
	return strncmp(jack_name, hdmi_dp, strlen(hdmi_dp)) == 0;
}

/* Checks if the given control name is in the supplied list of possible jack
 * control base names. */
static int is_jack_control_in_list(const char * const *list,
				   unsigned int list_length,
				   const char *control_name)
{
	unsigned int i;

	for (i = 0; i < list_length; i++)
		if (strncmp(control_name, list[i], strlen(list[i])) == 0)
			return 1;
	return 0;
}

/* Registers each poll fd (one per jack) with the system so that they are passed
 * to select in the main loop. */
static int add_jack_poll_fds(struct cras_alsa_jack_list *jack_list)
{
	struct pollfd *pollfds;
	nfds_t n;
	unsigned int i;
	int rc = 0;

	n = snd_hctl_poll_descriptors_count(jack_list->hctl);
	if (n == 0)
		return 0;

	pollfds = malloc(n * sizeof(*pollfds));
	if (pollfds == NULL)
		return -ENOMEM;

	n = snd_hctl_poll_descriptors(jack_list->hctl, pollfds, n);
	for (i = 0; i < n; i++) {
		struct jack_poll_fd *registered_fd;

		registered_fd = calloc(1, sizeof(*registered_fd));
		if (registered_fd == NULL) {
			rc = -ENOMEM;
			break;
		}
		registered_fd->fd = pollfds[i].fd;
		DL_APPEND(jack_list->registered_fds, registered_fd);
		rc = cras_system_add_select_fd(registered_fd->fd,
					       alsa_control_event_pending,
					       jack_list);
		if (rc < 0)
			break;
	}
	free(pollfds);
	return rc;
}

/* Cancels registration of each poll fd (one per jack) with the system. */
static void remove_jack_poll_fds(struct cras_alsa_jack_list *jack_list)
{
	struct jack_poll_fd *registered_fd;

	DL_FOREACH(jack_list->registered_fds, registered_fd) {
		cras_system_rm_select_fd(registered_fd->fd);
		DL_DELETE(jack_list->registered_fds, registered_fd);
		free(registered_fd);
	}
}

/* Looks for any JACK controls.  Monitors any found controls for changes and
 * decides to route based on plug/unlpug events. */
static int find_jack_controls(struct cras_alsa_jack_list *jack_list,
			      const char *device_name,
			      enum CRAS_STREAM_DIRECTION direction)
{
	int rc;
	snd_hctl_elem_t *elem;
	struct cras_alsa_jack *jack;
	const char *name;
	static const char * const output_jack_base_names[] = {
		"Headphone Jack",
		"Front Headphone Jack",
		"HDMI/DP",
	};
	static const char * const input_jack_base_names[] = {
		"Mic Jack",
	};
	static const char eld_control_name[] = "ELD";
	const char * const *jack_names;
	unsigned int num_jack_names;

	if (direction == CRAS_STREAM_OUTPUT) {
		jack_names = output_jack_base_names;
		num_jack_names = ARRAY_SIZE(output_jack_base_names);
	} else {
		jack_names = input_jack_base_names;
		num_jack_names = ARRAY_SIZE(input_jack_base_names);
	}

	rc = snd_hctl_open(&jack_list->hctl, device_name, SND_CTL_NONBLOCK);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to get hctl for %s", device_name);
		return rc;
	}
	rc = snd_hctl_nonblock(jack_list->hctl, 1);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to nonblock hctl for %s", device_name);
		return rc;
	}
	rc = snd_hctl_load(jack_list->hctl);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to load hctl for %s", device_name);
		return rc;
	}
	for (elem = snd_hctl_first_elem(jack_list->hctl); elem != NULL;
			elem = snd_hctl_elem_next(elem)) {
		snd_ctl_elem_iface_t iface;

		iface = snd_hctl_elem_get_interface(elem);
		if (iface != SND_CTL_ELEM_IFACE_CARD)
			continue;
		name = snd_hctl_elem_get_name(elem);
		if (!is_jack_control_in_list(jack_names, num_jack_names, name))
			continue;
		if (jack_device_index(name) != jack_list->device_index)
			continue;

		jack = cras_alloc_jack(0);
		if (jack == NULL)
			return -ENOMEM;
		jack->elem = elem;
		jack->jack_list = jack_list;
		DL_APPEND(jack_list->jacks, jack);

		syslog(LOG_DEBUG, "Found Jack: %s for %s", name, device_name);
		snd_hctl_elem_set_callback(elem, hctl_jack_cb);
		snd_hctl_elem_set_callback_private(elem, jack);

		if (direction == CRAS_STREAM_OUTPUT)
			jack->mixer_output =
				cras_alsa_mixer_get_output_matching_name(
					jack_list->mixer,
					jack_list->device_index,
					name);
		if (jack_list->ucm)
			jack->ucm_device =
				ucm_get_dev_for_jack(jack_list->ucm, name,
						     direction);

		if (jack->ucm_device && direction == CRAS_STREAM_INPUT) {
			char *control_name;
			control_name = ucm_get_cap_control(jack->jack_list->ucm,
						       jack->ucm_device);
			if (control_name)
				jack->mixer_input =
					cras_alsa_mixer_get_input_matching_name(
						jack_list->mixer,
						control_name);
		}

		if (jack->ucm_device) {
			jack->dsp_name = ucm_get_dsp_name(
				jack->jack_list->ucm, jack->ucm_device,
				direction);
			jack->override_type_name = ucm_get_override_type_name(
				jack->jack_list->ucm, jack->ucm_device);
		}
	}

	/* Look up ELD controls */
	DL_FOREACH(jack_list->jacks, jack) {
		name = snd_hctl_elem_get_name(jack->elem);
		if (!is_jack_hdmi_dp(name))
			continue;
		for (elem = snd_hctl_first_elem(jack_list->hctl); elem != NULL;
		     elem = snd_hctl_elem_next(elem)) {
			if (strcmp(snd_hctl_elem_get_name(elem),
				   eld_control_name))
				continue;
			if (snd_hctl_elem_get_device(elem)
			    != jack_list->device_index)
				continue;
			jack->eld_control = elem;
			break;
		}
	}

	/* If we have found jacks, have the poll fds passed to select in the
	 * main loop. */
	if (jack_list->jacks != NULL) {
		rc = add_jack_poll_fds(jack_list);
		if (rc < 0)
			return rc;
	}

	return 0;
}

/*
 * Exported Interface.
 */

struct cras_alsa_jack_list *cras_alsa_jack_list_create(
		unsigned int card_index,
		const char *card_name,
		unsigned int device_index,
		int check_gpio_jack,
		struct cras_alsa_mixer *mixer,
		snd_use_case_mgr_t *ucm,
		enum CRAS_STREAM_DIRECTION direction,
		jack_state_change_callback *cb,
		void *cb_data)
{
	struct cras_alsa_jack_list *jack_list;
	char device_name[6];

	if (direction != CRAS_STREAM_INPUT && direction != CRAS_STREAM_OUTPUT)
		return NULL;

	/* Enforce alsa limits. */
	if (card_index >= 32 || device_index >= 32) {
		syslog(LOG_ERR, "Jack List: Invalid card/dev %u/%u",
		       card_index, device_index);
		return NULL;
	}

	jack_list = (struct cras_alsa_jack_list *)calloc(1, sizeof(*jack_list));
	if (jack_list == NULL)
		return NULL;

	jack_list->change_callback = cb;
	jack_list->callback_data = cb_data;
	jack_list->mixer = mixer;
	jack_list->ucm = ucm;
	jack_list->device_index = device_index;

	snprintf(device_name, sizeof(device_name), "hw:%d", card_index);

	if (find_jack_controls(jack_list, device_name, direction) != 0) {
		cras_alsa_jack_list_destroy(jack_list);
		return NULL;
	}

	/*
	 * GPIO jacks are attached to the first input device or the first
	 * output device on the card.
	 */
	if (jack_list->jacks == NULL && check_gpio_jack)
		find_gpio_jacks(jack_list, card_index, card_name, direction);

	return jack_list;
}

void cras_alsa_jack_list_destroy(struct cras_alsa_jack_list *jack_list)
{
	struct cras_alsa_jack *jack;

	if (jack_list == NULL)
		return;
	remove_jack_poll_fds(jack_list);
	DL_FOREACH(jack_list->jacks, jack) {
		DL_DELETE(jack_list->jacks, jack);
		free(jack->ucm_device);

		free((void *)jack->edid_file);
		if (jack->display_info_timer)
			cras_tm_cancel_timer(cras_system_state_get_tm(),
					     jack->display_info_timer);

		if (jack->is_gpio) {
			free(jack->gpio.device_name);
			close(jack->gpio.fd);
		}

		if (jack->mixer_input)
			free(jack->mixer_input);

		if (jack->override_type_name)
			free((void *)jack->override_type_name);

		free((void *)jack->dsp_name);
		free(jack);
	}
	if (jack_list->hctl)
		snd_hctl_close(jack_list->hctl);
	free(jack_list);
}

struct cras_alsa_mixer_output *cras_alsa_jack_get_mixer_output(
		const struct cras_alsa_jack *jack)
{
	if (jack == NULL)
		return NULL;
	return jack->mixer_output;
}

struct mixer_volume_control *cras_alsa_jack_get_mixer_input(
		const struct cras_alsa_jack *jack)
{
	return jack ? jack->mixer_input : NULL;
}

void cras_alsa_jack_list_report(const struct cras_alsa_jack_list *jack_list)
{
	struct cras_alsa_jack *jack;

	if (jack_list == NULL)
		return;

	DL_FOREACH(jack_list->jacks, jack)
		if (jack->is_gpio)
			gpio_switch_initial_state(jack);
		else
			hctl_jack_cb(jack->elem, 0);
}

const char *cras_alsa_jack_get_name(const struct cras_alsa_jack *jack)
{
	if (jack == NULL)
		return NULL;
	if (jack->is_gpio)
		return jack->gpio.device_name;
	return snd_hctl_elem_get_name(jack->elem);
}

void cras_alsa_jack_update_monitor_name(const struct cras_alsa_jack *jack,
					char *name_buf,
					unsigned int buf_size)
{
	snd_ctl_elem_value_t *elem_value;
	snd_ctl_elem_info_t *elem_info;
	const char *buf = NULL;
	int count;
	int mnl = 0;

	if (!jack->eld_control)
		return;

	snd_ctl_elem_info_alloca(&elem_info);
	if (snd_hctl_elem_info(jack->eld_control, elem_info) < 0)
		goto fallback_jack_name;

	count = snd_ctl_elem_info_get_count(elem_info);
	if (count <= ELD_MNL_OFFSET)
		goto fallback_jack_name;

	snd_ctl_elem_value_alloca(&elem_value);
	if (snd_hctl_elem_read(jack->eld_control, elem_value) < 0)
		goto fallback_jack_name;

	buf = (const char *)snd_ctl_elem_value_get_bytes(elem_value);
	mnl = buf[ELD_MNL_OFFSET] & ELD_MNL_MASK;

	if (count < ELD_MONITOR_NAME_OFFSET + mnl)
		goto fallback_jack_name;

	/* Note that monitor name string does not contain terminate character.
	 * Check monitor name length with name buffer size.
	 */
	if (mnl >= buf_size)
		mnl = buf_size - 1;
	strncpy(name_buf, buf + ELD_MONITOR_NAME_OFFSET, mnl);
	name_buf[mnl] = '\0';

	return;

fallback_jack_name:
	buf = cras_alsa_jack_get_name(jack);
	strncpy(name_buf, buf, buf_size - 1);

	return;
}

void cras_alsa_jack_update_node_type(const struct cras_alsa_jack *jack,
				     enum CRAS_NODE_TYPE *type)
{
	if (!jack->override_type_name)
		return;
	if (!strcmp(jack->override_type_name, "Internal Speaker"))
		*type = CRAS_NODE_TYPE_INTERNAL_SPEAKER;
	return;
}

const char *cras_alsa_jack_get_dsp_name(const struct cras_alsa_jack *jack)
{
	if (jack == NULL)
		return NULL;
	return jack->dsp_name;
}

void cras_alsa_jack_enable_ucm(const struct cras_alsa_jack *jack, int enable)
{
	if (jack && jack->ucm_device)
		ucm_set_enabled(jack->jack_list->ucm, jack->ucm_device, enable);
}

int cras_alsa_jack_exists(unsigned int card_index, const char *jack_name)
{
	char device_name[6];
	snd_hctl_t *hctl = NULL;
	snd_hctl_elem_t *elem;
	int rc;
	int result = 0;

	snprintf(device_name, sizeof(device_name), "hw:%d", card_index);

	rc = snd_hctl_open(&hctl, device_name, SND_CTL_NONBLOCK);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to get hctl for %s", device_name);
		goto out;
	}
	rc = snd_hctl_nonblock(hctl, 1);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to nonblock hctl for %s", device_name);
		goto out;
	}
	rc = snd_hctl_load(hctl);
	if (rc < 0) {
		syslog(LOG_ERR, "failed to load hctl for %s", device_name);
		goto out;
	}

	for (elem = snd_hctl_first_elem(hctl); elem != NULL;
			elem = snd_hctl_elem_next(elem)) {
		snd_ctl_elem_iface_t iface;
		const char *name;

		iface = snd_hctl_elem_get_interface(elem);
		if (iface != SND_CTL_ELEM_IFACE_CARD)
			continue;
		name = snd_hctl_elem_get_name(elem);
		if (strcmp(name, jack_name) == 0) {
			result = 1;
			break;
		}
	}

out:
	if (hctl)
		snd_hctl_close(hctl);
	return result;
}
