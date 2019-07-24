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
#include <uuid/uuid.h>
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
#include "navit.h"
#include <unistd.h>
#include "zlib.h"
#include <stdbool.h>
#ifdef _WIN32
#include <Rpc.h>
#endif


/**
 * @brief Default config data for the broker connection
 */
#define ADDRESS     "tcp://localhost:1883"
#define TOPIC       "navit/traff"
#define QOS         1
#define TIMEOUT     10000L

volatile bool connected = FALSE;
volatile MQTTClient_deliveryToken deliveredtoken;

/**
 * @brief Config data of the plugin instance.
 */
struct mqtt {
    gchar * brokerurl;
    bool compressed;
    gchar * topic;
    gchar * user;
    gchar * passwd;
};

/**
 * @brief Stores information about the plugin instance.
 */
struct traffic_priv {
    struct navit * nav; /**< The navit instance */
    struct callback * cbid; /**< The callback function for TraFF feeds **/
    struct mqtt * mqtt;
};

static void traffic_traff_mqtt_on_feed_received(struct traffic_priv * this_, char * feed);

static void traffic_traff_mqtt_receive(struct traffic_priv * this_);
void delivered(void *context, MQTTClient_deliveryToken dt);
int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message);
void connlost(void *context, char *cause);

/**
 * @brief Called by MQTT thread when a message is delivered.
 *
 * @param context The client context
 * @param dt 	  The token of the message delivered
 */
void delivered(void *context, MQTTClient_deliveryToken dt) {
    dbg(lvl_debug, "MQTT: Message with token value %d delivery confirmed", dt);
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
int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message) {

    char* payloadptr;

    dbg(lvl_debug, "MQTT: Message arrived");
    dbg(lvl_debug, "topic: %s", topicName);

    payloadptr = message->payload;
    char traffmsg[message->payloadlen + 1];

    if (((struct traffic_priv *) context)->mqtt->compressed != 0) {
        uLongf len = message->payloadlen;

        /* inflate the message */
        int result = uncompress((Bytef *) traffmsg, &len, (Bytef *) payloadptr, message->payloadlen);

        if (result != Z_OK) {
            dbg(lvl_debug, "Decomp error\n%i", result);
        }

    } else {
        char * plptr = message->payload;

        int i = 0;

        for (i = 0; i < message->payloadlen; i++) {
            traffmsg[i] = (char) *plptr++;
        }

        traffmsg[i] = 0;
    }

    dbg(lvl_debug, "\n%s", traffmsg);

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
    dbg(lvl_debug, "\nMQTT: Connection lost\n");
    dbg(lvl_debug, "     cause: %s\n\n", cause);

    connected = FALSE;

}

struct traffic_message ** traffic_traff_mqtt_get_messages(struct traffic_priv * this_);

/**
 * @brief Returns an empty traffic report.
 *
 * @return Always `NULL`
 */
struct traffic_message ** traffic_traff_mqtt_get_messages(struct traffic_priv * this_) {
    return NULL;
}

/**
 * @brief The methods implemented by this plugin
 */
static struct traffic_methods traffic_traff_mqtt_meth = { traffic_traff_mqtt_get_messages, };

/**
 * @brief Called when a new TraFF feed is received.
 *
 * @param this_ Private data for the module instance
 * @param feed Feed data in string form
 */
static void traffic_traff_mqtt_on_feed_received(struct traffic_priv * this_, char * feed) {
    struct attr * attr;
    struct attr_iter * a_iter;
    struct traffic * traffic = NULL;
    struct traffic_message ** messages;

    dbg(lvl_debug, "enter");
    attr = g_new0(struct attr, 1);
    //a_iter = g_new0(struct attr_iter, 1);
    a_iter = navit_attr_iter_new();
    if (navit_get_attr(this_->nav, attr_traffic, attr, a_iter))
        traffic = (struct traffic *) attr->u.navit_object;
    navit_attr_iter_destroy(a_iter);
    g_free(attr);

    if (!traffic) {
        dbg(lvl_debug, "failed to obtain traffic instance");
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

    char *uuid;

#if  !WIN32
    uuid_t binuuid;
    uuid_generate_random(binuuid);
    uuid = malloc(37);
    uuid_unparse_upper(binuuid, uuid);
#else
    UUID winuuid;
    char str[40];
    UuidCreate(&winuuid);
    sprintf(str, "%l", winuuid.Data1);
    uuid=&str;
#endif

    rc = MQTTClient_create(&client, (this_->mqtt->brokerurl != 0) ? this_->mqtt->brokerurl : ADDRESS, uuid,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);

    if(rc!=MQTTCLIENT_SUCCESS) {
        dbg(lvl_error, "MQTT: Failed to create MQTT client. Return code %d\n\n", rc);
        exit(1);
    }

    if (this_->mqtt->user && this_->mqtt->passwd) {
        conn_opts.username = this_->mqtt->user;
        conn_opts.password = this_->mqtt->passwd;
    }

    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    MQTTClient_setCallbacks(client, this_, connlost, msgarrvd, delivered);
    int delay = 1;

    while (TRUE) {

        if (!connected) {

            if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
                dbg(lvl_debug, "MQTT: Failed to connect to %s, return code %d. Try reconnect in %i seconds\n\n", ADDRESS,
                    rc, delay + 1);
                if (delay < 61)
                    delay++;
            } else {

                connected = TRUE;
                delay = 1;

                dbg(lvl_debug, "MQTT: Subscribing to topic %s for client %s using QoS%d at %s\n\n",
                    (this_->mqtt->topic!=0)?this_->mqtt->topic:TOPIC, uuid, QOS,
                    (this_->mqtt->brokerurl != 0) ? this_->mqtt->brokerurl : ADDRESS);
                MQTTClient_subscribe(client, (this_->mqtt->topic != 0) ? this_->mqtt->topic : TOPIC, QOS);
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

    pthread_create(&subscriber, NULL, (void * (*)(void *)) traffic_traff_mqtt_receive, this_);

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
static struct traffic_priv * traffic_traff_mqtt_new(struct navit *nav, struct traffic_methods *meth,
        struct attr **attrs, struct callback_list *cbl) {

    struct traffic_priv *ret;
    struct attr * attr;

    dbg(lvl_debug, "enter");

    ret = g_new0(struct traffic_priv, 1);
    ret->mqtt = g_new0(struct mqtt, 1);

    if ((attr = attr_search(attrs, NULL, attr_mqtt_brokerurl))) {
        ret->mqtt->brokerurl = g_strdup(attr->u.str);
        dbg(lvl_debug, "found broker url %s\n", ret->mqtt->brokerurl);
    }

    if ((attr = attr_search(attrs, NULL, attr_mqtt_compressed))) {
        ret->mqtt->compressed = (bool) (attr->u.num);
        dbg(lvl_debug, "found compressed %d\n", ret->mqtt->compressed);
    }

    if ((attr = attr_search(attrs, NULL, attr_mqtt_topic))) {
        ret->mqtt->topic = g_strdup(attr->u.str);
        dbg(lvl_debug, "found topic %s\n", ret->mqtt->topic);
    }

    if ((attr = attr_search(attrs, NULL, attr_mqtt_user))) {
        ret->mqtt->user = g_strdup(attr->u.str);
        dbg(lvl_debug, "found user %s\n", ret->mqtt->user);
    }

    if ((attr = attr_search(attrs, NULL, attr_mqtt_passwd))) {
        ret->mqtt->passwd = g_strdup(attr->u.str);
        dbg(lvl_debug, "found passwd %s\n", ret->mqtt->passwd);
    }

    ret->nav = nav;
    ret->cbid = callback_new_1(callback_cast(traffic_traff_mqtt_on_feed_received), ret);
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
