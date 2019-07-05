/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2018 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

/**
 * @file traffic_traff_mqtt.c
 *
 * @brief The TraFF plugin using MQTT
 *
 * This plugin receives TraFF feeds via MQTT broadcasts.
 */

#include <string.h>
#include <time.h>

#ifdef _POSIX_C_SOURCE
#include <sys/types.h>
#endif
#include "glib_slice.h"
#include "config.h"
#include "item.h"
#include "attr.h"
#include "coord.h"
#include "xmlconfig.h"
#include "traffic.h"
#include "plugin.h"
#include "callback.h"
#include "debug.h"
#include <pthread.h>
#include "MQTTClient.h"
#include <stdbool.h>

#define ADDRESS     "tcp://localhost:1883"
#define CLIENTID    "navit"
#define TOPIC       "navit/traff"
#define QOS         1
#define TIMEOUT     10000L

volatile bool connected = FALSE;

volatile MQTTClient_deliveryToken deliveredtoken;

static void traffic_traff_mqtt_on_feed_received(struct traffic_priv * this_,
		char * feed);

static void traffic_traff_mqtt_receive(struct traffic_priv * this_);

/**
 * @brief Called by MQTT thread when a message is delivered.
 *
 * @param context The client context
 * @param dt 	  The token of the message delivered
 */
void delivered(void *context, MQTTClient_deliveryToken dt) {
	printf("MQTT: Message with token value %d delivery confirmed\n", dt);
	deliveredtoken = dt;
}

/**
 * @brief Called by MQTT thread when a new message arrived.
 *
 * @param context The client context
 * @param topicName The name of the topic received
 * @param topicLen	The length of the topic
 * @param message	The message
 */
int msgarrvd(void *context, char *topicName, int topicLen,
		MQTTClient_message *message) {
	int i;
	char* payloadptr;
	printf("MQTT: Message arrived\n");
	printf("     topic: %s\n", topicName);
	printf("   message: ");
	payloadptr = message->payload;
	char traffmsg[message->payloadlen + 1];
	for (i = 0; i < message->payloadlen; i++) {
		putchar(*payloadptr);
		traffmsg[i] = *payloadptr++;
	}
	putchar('\n');
	traffmsg[i] = 0;

	payloadptr = message->payload;

	traffic_traff_mqtt_on_feed_received(context, traffmsg);

	MQTTClient_freeMessage(&message);
	MQTTClient_free(topicName);
	return 1;
}

/**
 * @brief Called by MQTT thread when the connection to the broker is lost.
 *
 * @param context The client context
 * @param cause   The cause for the connection loss
 */
void connlost(void *context, char *cause) {
	printf("\nMQTT: Connection lost\n");
	printf("     cause: %s\n\n", cause);

	connected = FALSE;

}

/**
 * @brief Stores information about the plugin instance.
 */
struct traffic_priv {
	struct navit * nav; /**< The navit instance */
	struct callback * cbid; /**< The callback function for TraFF feeds **/
};

struct traffic_message ** traffic_traff_mqtt_get_messages(
		struct traffic_priv * this_);

/**
 * @brief Returns an empty traffic report.
 *
 * @return Always `NULL`
 */
struct traffic_message ** traffic_traff_mqtt_get_messages(
		struct traffic_priv * this_) {
	return NULL;
}

/**
 * @brief The methods implemented by this plugin
 */
static struct traffic_methods traffic_traff_mqtt_meth = {
		traffic_traff_mqtt_get_messages, };

/**
 * @brief Called when a new TraFF feed is received.
 *
 * @param this_ Private data for the module instance
 * @param feed Feed data in string form
 */
static void traffic_traff_mqtt_on_feed_received(struct traffic_priv * this_,
		char * feed) {
	struct attr * attr;

	struct mapset_handle {
		GList *l; /**< Pointer to the current (next) map */
	};

	struct attr_iter {
		void *iter;
		union {
			GList *list;
			struct mapset_handle *mapset_handle;
		} u;
	};

	struct attr_iter * a_iter;
	struct traffic * traffic = NULL;
	struct traffic_message ** messages;

	dbg(lvl_debug, "enter");
	attr = g_new0(struct attr, 1);
	a_iter = g_new0(struct attr_iter, 1);
	if (navit_get_attr(this_->nav, attr_traffic, attr, a_iter))
		traffic = (struct traffic *) attr->u.navit_object;
	navit_attr_iter_destroy(a_iter);
	g_free(attr);

	if (!traffic) {
		dbg(lvl_error, "failed to obtain traffic instance");
		return;
	}

	dbg(lvl_debug, "processing traffic feed:\n%s", feed);
	messages = traffic_get_messages_from_xml_string(traffic, feed);
	if (messages) {
		dbg(lvl_debug, "got messages from feed, processing");
		traffic_process_messages(traffic, messages);
		g_free(messages);
	}
}

/**
 * @brief Called by pthread_create in traffic_traff_mqtt_init
 *
 * @param this_ Private data for the module instance
 *
 */
static void traffic_traff_mqtt_receive(struct traffic_priv * this_) {

	MQTTClient client;
	MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
	int rc;
	int ch;
	MQTTClient_create(&client, ADDRESS, CLIENTID,
	MQTTCLIENT_PERSISTENCE_NONE, NULL);
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	MQTTClient_setCallbacks(client, this_, connlost, msgarrvd, delivered);
	int delay=1;

	while (TRUE) {

		if (!connected) {

			if ((rc = MQTTClient_connect(client, &conn_opts))
					!= MQTTCLIENT_SUCCESS) {
				printf("MQTT: Failed to connect to %s, return code %d. Try reconnect in %i seconds\n\n", ADDRESS, rc, delay + 1);
				if(delay < 61)
					delay++;
			} else {

				connected = TRUE;
				delay=1;

				printf("MQTT: Subscribing to topic %s for client %s using QoS%d at %s\n\n",
				TOPIC, CLIENTID, QOS, ADDRESS);
				MQTTClient_subscribe(client, TOPIC, QOS);
			}
		}

		sleep(delay);
	}

}
/**
 * @brief Initializes a traff_mqtt plugin
 *
 * @return True on success, false on failure
 */
static int traffic_traff_mqtt_init(struct traffic_priv * this_) {
	pthread_t subscriber;

	pthread_create(&subscriber, NULL, traffic_traff_mqtt_receive, this_);

	return 1;
}

/**
 * @brief Registers a new traff_mqtt traffic plugin
 *
 * @param nav The navit instance
 * @param meth Receives the traffic methods
 * @param attrs The attributes for the map
 * @param cbl
 *
 * @return A pointer to a `traffic_priv` structure for the plugin instance
 */
static struct traffic_priv * traffic_traff_mqtt_new(struct navit *nav,
		struct traffic_methods *meth, struct attr **attrs,
		struct callback_list *cbl) {
	struct traffic_priv *ret;

	dbg(lvl_debug, "enter");

	ret = g_new0(struct traffic_priv, 1);
	ret->nav = nav;
	ret->cbid = callback_new_1(
			callback_cast(traffic_traff_mqtt_on_feed_received), ret);
	/* TODO populate members, if any */
	*meth = traffic_traff_mqtt_meth;

	traffic_traff_mqtt_init(ret);

	return ret;
}

/**
 * @brief Initializes the traffic plugin.
 *
 * This function is called once on startup.
 */
void plugin_init(void) {
	dbg(lvl_debug, "enter");

	plugin_register_category_traffic("traff_mqtt", traffic_traff_mqtt_new);
}