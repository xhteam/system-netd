/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/wireless.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define LOG_TAG "SoftapController"
#include <cutils/log.h>
#include <netutils/ifc.h>
#include <private/android_filesystem_config.h>
#include "wifi.h"

#include "SoftapController.h"


#ifdef ATH_WIFI
#include <athdefs.h>
#include <a_types.h>
#include <a_osapi.h>
#include <wmi.h>
#include <athdrv_linux.h>
#include <athtypes_linux.h>
#include <ieee80211.h>
#include <ieee80211_ioctl.h>
#include "NetlinkManager.h"
#include "ResponseCode.h"

#include "private/android_filesystem_config.h"
#include "cutils/properties.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#endif

#include <sys/_system_properties.h>
#include "libhostapd_client/wpa_ctrl.h"

static const char IFACE_DIR[]           = "/dev/socket/hostapd_";
static const char HOSTAPD_NAME[]     = "hostapd";
static const char HOSTAPD_CONFIG_TEMPLATE[]= "/system/etc/wifi/hostapd.conf";
static const char HOSTAPD_CONFIG_FILE[]    = "/data/misc/wifi/hostapd.conf";
static const char HOSTAPD_PROP_NAME[]      = "init.svc.hostapd";

#define WIFI_TEST_INTERFACE     "sta"
#define WIFI_DEFAULT_BI         100         /* in TU */
#define WIFI_DEFAULT_DTIM       1           /* in beacon */
#define WIFI_DEFAULT_CHANNEL    6
#define WIFI_DEFAULT_MAX_STA    8
#define WIFI_DEFAULT_PREAMBLE   0
#define WIFI_AP_INTERFACE       "wlan0"


static int if_rename(int sock, const char *oldName, const char *newName)
{
    int ret;
    int flags;
    NetlinkManager *nm;
    struct ifreq ifr;
    do {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, oldName, IFNAMSIZ);
	if ((ret=ioctl(sock, SIOCGIFFLAGS, (caddr_t)&ifr)) <0) {
            break;
        }
        flags = (ifr.ifr_flags & 0xffff);
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, oldName, IFNAMSIZ);
        ifr.ifr_flags = (flags & ~IFF_UP);
	if ((ret=ioctl(sock, SIOCSIFFLAGS, (caddr_t)&ifr))<0) {
            break;
        }
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, oldName, IFNAMSIZ);
        strncpy(ifr.ifr_newname, newName, IFNAMSIZ);
        if ((ret = ioctl(sock, SIOCSIFNAME, &ifr)) < 0) {
            break;
        }
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, newName, IFNAMSIZ);
        ifr.ifr_flags = (flags | IFF_UP);
        if ((ret=ioctl(sock, SIOCSIFFLAGS, (caddr_t)&ifr))<0) {
            break;
        }
        nm = NetlinkManager::Instance();
        if (nm) {
            char msg[255];
            snprintf(msg, sizeof(msg), "Iface removed %s", oldName);
            nm->getBroadcaster()->sendBroadcast(ResponseCode::InterfaceChange,
                                                 msg, false);
            snprintf(msg, sizeof(msg), "Iface added %s", newName);
            nm->getBroadcaster()->sendBroadcast(ResponseCode::InterfaceChange,
                                                 msg, false);
        }
    } while (0);
    return ret;
}

int ensure_config_file_exists()
{
    char buf[2048];
    int srcfd, destfd;
    int nread;

    if (access(HOSTAPD_CONFIG_FILE, R_OK|W_OK) == 0) {
        return 0;
    } else if (errno != ENOENT) {
        LOGE("Cannot access \"%s\": %s", HOSTAPD_CONFIG_FILE, strerror(errno));
        return -1;
    }

    srcfd = open(HOSTAPD_CONFIG_TEMPLATE, O_RDONLY);
    if (srcfd < 0) {
        LOGE("Cannot open \"%s\": %s", HOSTAPD_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = open(HOSTAPD_CONFIG_FILE, O_CREAT|O_WRONLY, 0660);
    if (destfd < 0) {
        close(srcfd);
        LOGE("Cannot create \"%s\": %s", HOSTAPD_CONFIG_FILE, strerror(errno));
        return -1;
    }

    while ((nread = read(srcfd, buf, sizeof(buf))) != 0) {
        if (nread < 0) {
            LOGE("Error reading \"%s\": %s", HOSTAPD_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(HOSTAPD_CONFIG_FILE);
            return -1;
        }
        write(destfd, buf, nread);
    }

    close(destfd);
    close(srcfd);

    if (chown(HOSTAPD_CONFIG_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             HOSTAPD_CONFIG_FILE, AID_WIFI, strerror(errno));
        unlink(HOSTAPD_CONFIG_FILE);
        return -1;
    }

    return 0;
}

int wifi_start_hostapd()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0;
#endif

    /* Check whether already running */
    if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        return 0;
    }


    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(HOSTAPD_PROP_NAME);
    if (pi != NULL) {
        serial = pi->serial;
    }
#endif
    property_set("ctl.start", HOSTAPD_NAME);
    sched_yield();

    while (count-- > 0) {
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(HOSTAPD_PROP_NAME);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                return 0;
            } else if (pi->serial != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                LOGD("hostapd stoped");
                return -1;
            }
        }
#else
        if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "running") == 0)
                return 0;
        }
#endif
        usleep(100000);
    }
    LOGD("START HOSTAPD FAILED");
    return -1;
}

int wifi_stop_hostapd()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    /* Check whether hostapd already stopped */
    if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

    property_set("ctl.stop", HOSTAPD_NAME);
    sched_yield();

    while (count-- > 0) {
        if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    return -1;
}


#endif /* ATH_WIFI */

SoftapController::SoftapController() {
    mPid = 0;
    mSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (mSock < 0)
        LOGE("Failed to open socket");
    memset(mIface, 0, sizeof(mIface));
}

SoftapController::~SoftapController() {
    if (mSock >= 0)
        close(mSock);
}

int SoftapController::setCommand(char *iface, const char *fname, unsigned buflen) {
#ifdef HAVE_HOSTAPD
    return 0;
#else
    char tBuf[SOFTAP_MAX_BUFFER_SIZE];
    struct iwreq wrq;
    struct iw_priv_args *priv_ptr;
    int i, j, ret;
    int cmd = 0, sub_cmd = 0;

    strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
    wrq.u.data.pointer = tBuf;
    wrq.u.data.length = sizeof(tBuf) / sizeof(struct iw_priv_args);
    wrq.u.data.flags = 0;
    if ((ret = ioctl(mSock, SIOCGIWPRIV, &wrq)) < 0) {
        LOGE("SIOCGIPRIV failed: %d", ret);
        return ret;
    }

    priv_ptr = (struct iw_priv_args *)wrq.u.data.pointer;
    for(i=0; i < wrq.u.data.length;i++) {
        if (strcmp(priv_ptr[i].name, fname) == 0) {
            cmd = priv_ptr[i].cmd;
            break;
        }
    }

    if (i == wrq.u.data.length) {
        LOGE("iface:%s, fname: %s - function not supported", iface, fname);
        return -1;
    }

    if (cmd < SIOCDEVPRIVATE) {
        for(j=0; j < i; j++) {
            if ((priv_ptr[j].set_args == priv_ptr[i].set_args) &&
                (priv_ptr[j].get_args == priv_ptr[i].get_args) &&
                (priv_ptr[j].name[0] == '\0'))
                break;
        }
        if (j == i) {
            LOGE("iface:%s, fname: %s - invalid private ioctl", iface, fname);
            return -1;
        }
        sub_cmd = cmd;
        cmd = priv_ptr[j].cmd;
    }

    strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
    if ((buflen == 0) && (*mBuf != 0))
        wrq.u.data.length = strlen(mBuf) + 1;
    else
        wrq.u.data.length = buflen;
    wrq.u.data.pointer = mBuf;
    wrq.u.data.flags = sub_cmd;
    ret = ioctl(mSock, cmd, &wrq);
    return ret;
#endif
}

int SoftapController::startDriver(char *iface) {
    int ret;

    if (mSock < 0) {
        LOGE("Softap driver start - failed to open socket");
        return -1;
    }
    if (!iface || (iface[0] == '\0')) {
        LOGD("Softap driver start - wrong interface");
        iface = mIface;
    }
#ifdef ATH_WIFI
    /* Before starting the daemon, make sure its config file exists */
    ret = ensure_config_file_exists();
    if (ret < 0) {
        LOGE("Softap driver start - configuration file missing");
        return -1;
    }
#else
    *mBuf = 0;
    ret = setCommand(iface, "START");
    if (ret < 0) {
        LOGE("Softap driver start: %d", ret);
        return ret;
    }
#endif
#ifdef HAVE_HOSTAPD
    ifc_init();
    ret = ifc_up(iface);
    ifc_close();
#endif
    usleep(AP_DRIVER_START_DELAY);
    LOGD("Softap driver start: %d", ret);
    return ret;
}

int SoftapController::stopDriver(char *iface) {
    int ret;

    if (mSock < 0) {
        LOGE("Softap driver stop - failed to open socket");
        return -1;
    }
    if (!iface || (iface[0] == '\0')) {
        LOGD("Softap driver stop - wrong interface");
        iface = mIface;
    }
    
#ifdef HAVE_HOSTAPD
    ifc_init();
    ret = ifc_down(iface);
    ifc_close();
    if (ret < 0) {
        LOGE("Softap %s down: %d", iface, ret);
    }
#endif
#ifdef ATH_WIFI
    ret = 0;
#else
    *mBuf = 0;
    ret = setCommand(iface, "STOP");
#endif
    LOGD("Softap driver stop: %d", ret);
    return ret;
}

int SoftapController::startSoftap() {
    pid_t pid = 1;
    int ret = 0;

    if (mPid) {
        LOGE("Softap already started");
        return 0;
    }
    if (mSock < 0) {
        LOGE("Softap startap - failed to open socket");
        return -1;
    }
#ifdef HAVE_HOSTAPD
    if ((pid = fork()) < 0) {
        LOGE("fork failed (%s)", strerror(errno));
        return -1;
    }
#endif
#ifdef ATH_WIFI 

    if (mIface[0] == '\0') {
        LOGE("Softap startap - wrong interface");
        return -1;
    }

    mPid = 1;
    usleep(AP_BSS_START_DELAY);

    return ret;
#else /* ATH_WIFI */
    if (!pid) {
#ifdef HAVE_HOSTAPD
        ensure_entropy_file_exists();
        if (execl("/system/bin/hostapd", "/system/bin/hostapd",
                  "-e", WIFI_ENTROPY_FILE,
                  HOSTAPD_CONF_FILE, (char *) NULL)) {
            LOGE("execl failed (%s)", strerror(errno));
        }
#endif
        LOGE("Should never get here!");
        return -1;
    } else {
        *mBuf = 0;
        ret = setCommand(mIface, "AP_BSS_START");
        if (ret) {
            LOGE("Softap startap - failed: %d", ret);
        }
        else {
           mPid = pid;
           LOGD("Softap startap - Ok");
           usleep(AP_BSS_START_DELAY);
        }
    }
    return ret;
#endif
}

int SoftapController::stopSoftap() {
    int ret;
#ifdef ATH_WIFI 
    struct ifreq ifr;
    char ifname[PROPERTY_VALUE_MAX];
    struct iwreq wrq;
    int fnum;
#endif
    if (mPid == 0) {
        LOGE("Softap already stopped");
        return 0;
    }

#ifdef HAVE_HOSTAPD
    LOGD("Stopping Softap service");
    kill(mPid, SIGTERM);
    waitpid(mPid, NULL, 0);
#endif
    if (mSock < 0) {
        LOGE("Softap stopap - failed to open socket");
        return -1;
    }
#ifdef ATH_WIFI 
    if (mIface[0] == '\0') {
        LOGE("Softap startap - wrong interface");
        return -1;
    }

    ret = wifi_stop_hostapd();

    if (ret < 0) {
        LOGE("Softap stopap - stoping hostapd fails");
        return -1;
    }

    property_get("wifi.interface", ifname, "wlan0");

    /* Rename AP interface back to station interface name*/
    if ((ret = if_rename(mSock, WIFI_AP_INTERFACE , ifname)) < 0) {
        LOGE("Softap stopap - AR6000_IOCTL remove ap interface failed: %d", ret);
        return -1;
    }

    /* Stop AP mode in 3 steps */

    /* Step #1: iwconfig mode managed */
    memset(&wrq, 0, sizeof(wrq));
    strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
    wrq.u.mode = IW_MODE_INFRA;

    if ((ret = ioctl(mSock, SIOCSIWMODE, &wrq)) < 0) {
        LOGE("Softap stopap - AR6000_IOCTL failed: %d", ret);
        return -1;
    }

    /* Step #2: iwconfig essid abcdefghijklmnopqrstuvwxyz */
    memset(&wrq, 0, sizeof(wrq));
    strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
    wrq.u.essid.flags = 1; /* SSID active */
    strcpy(mBuf, "abcdefghijklmnopqrstuvwxyz");    
    wrq.u.essid.pointer = (caddr_t *)mBuf;
    wrq.u.essid.length = strlen(mBuf); 
    if ((ret = ioctl(mSock, SIOCSIWESSID, &wrq)) < 0) {
        LOGE("Softap stopap - AR6000_IOCTL failed: %d", ret);
        return -1;
    }

    /* Step #3: iwconfig essid off */
    mBuf[0] = '\0';
    wrq.u.essid.flags = 0; /* SSID active */
    wrq.u.essid.pointer = (caddr_t *)mBuf;
    wrq.u.essid.length = 0; /* Put some length */
    if ((ret = ioctl(mSock, SIOCSIWESSID, &wrq)) < 0) {
        LOGE("Softap stopap - AR6000_IOCTL failed: %d", ret);
        return -1;
    }  

    memset(mIface, 0, sizeof(mIface));
#else
    *mBuf = 0;
    ret = setCommand(mIface, "AP_BSS_STOP");
#endif
    mPid = 0;
    LOGD("Softap service stopped: %d", ret);
    usleep(AP_BSS_STOP_DELAY);
    return ret;
}

bool SoftapController::isSoftapStarted() {
    return (mPid != 0 ? true : false);
}

int SoftapController::addParam(int pos, const char *cmd, const char *arg)
{
    if (pos < 0)
        return pos;
    if ((unsigned)(pos + strlen(cmd) + strlen(arg) + 1) >= sizeof(mBuf)) {
        LOGE("Command line is too big");
        return -1;
    }
    pos += sprintf(&mBuf[pos], "%s=%s,", cmd, arg);
    return pos;
}

/*
 * Arguments:
 *      argv[2] - wlan interface
 *      argv[3] - softap interface
 *      argv[4] - SSID
 *	argv[5] - Security
 *	argv[6] - Key
 *	argv[7] - Channel
 *	argv[8] - Preamble
 *	argv[9] - Max SCB
 */
int SoftapController::setSoftap(int argc, char *argv[]) {
#ifdef ATH_WIFI 
    int fd;
    char buf[1024];
    int len;
#else
    char psk_str[2*SHA256_DIGEST_LENGTH+1];
#endif /* ATH_WIFI */
    int ret, i = 0;
    char *ssid, *iface;

    if (mSock < 0) {
        LOGE("Softap set - failed to open socket");
        return -1;
    }
    if (argc < 4) {
        LOGE("Softap set - missing arguments");
        return -1;
    }

#ifdef ATH_WIFI
    ret = 0;
		
		strncpy(mIface, argv[3], sizeof(mIface));
    fd = open(HOSTAPD_CONFIG_FILE, O_CREAT|O_WRONLY|O_TRUNC, 0660);
    if (fd < 0) {
        LOGE("Cannot create \"%s\": %s", HOSTAPD_CONFIG_FILE, strerror(errno));
        return -1;
    }
    len = snprintf(buf, sizeof(buf), "interface=%s\n", mIface);
    write(fd, buf, len);
    len = snprintf(buf, sizeof(buf), "ctrl_interface=%s\n", mIface);
    write(fd, buf, len);
    if (argc > 4) {
        len = snprintf(buf, sizeof(buf), "ssid=%s\n",argv[4]);
    } else {
        len = snprintf(buf, sizeof(buf), "ssid=AndroidAP\n");
    }
    /* set open auth */
    write(fd, buf, len);
    len = snprintf(buf, sizeof(buf), "auth_algs=1\n");
    write(fd, buf, len);
    {
        int max_sta = (argc > 9) ? atoi(argv[9]) : WIFI_DEFAULT_MAX_STA;
        if (max_sta < 1 || max_sta >WIFI_DEFAULT_MAX_STA) {
            max_sta = WIFI_DEFAULT_MAX_STA;
        }
        len = snprintf(buf, sizeof(buf), "max_num_sta=%d\n", max_sta);
        write(fd, buf, len);
    }
    len = snprintf(buf, sizeof(buf), "beacon_int=%d\n",WIFI_DEFAULT_BI);
    write(fd, buf, len);
    len = snprintf(buf, sizeof(buf), "dtim_period=%d\n",WIFI_DEFAULT_DTIM);
    write(fd, buf, len);
    if (argc > 5) {
        if (strncmp(argv[5], "wpa2-psk", 8) == 0) {
            len = snprintf(buf, sizeof(buf), "wpa=2\n");
            write(fd, buf, len);
            len = snprintf(buf, sizeof(buf), "wpa_key_mgmt=WPA-PSK\n");
            write(fd, buf, len);
            len = snprintf(buf, sizeof(buf), "wpa_pairwise=CCMP\n");
            write(fd, buf, len);
            if (argc > 6) {
                len = snprintf(buf, sizeof(buf), "wpa_passphrase=%s\n",argv[6]);
                write(fd, buf, len);
            } else {
                len = snprintf(buf, sizeof(buf), "wpa_passphrase=12345678\n");
                write(fd, buf, len);
            }
        }
    }
    if (argc > 7) {
        len = snprintf(buf, sizeof(buf), "channel_num=%s\n",argv[7]);
        write(fd, buf, len);
    } else {
        len = snprintf(buf, sizeof(buf), "channel_num=%d\n",WIFI_DEFAULT_CHANNEL);
        write(fd, buf, len);
    }
    if (argc > 8) {
        len = snprintf(buf, sizeof(buf), "preamble=%s\n",argv[8]);
        write(fd, buf, len);
    } else {
        len = snprintf(buf, sizeof(buf), "preamble=%d\n",WIFI_DEFAULT_PREAMBLE);
        write(fd, buf, len);
    }


    close(fd);

    if (isSoftapStarted()) {
        /* Restart hostapd */
        ret = wifi_stop_hostapd();
        if (ret < 0) {
            LOGE("Softap Softap set - stoping hostapd fails");
            return -1;
        }

        ret = wifi_start_hostapd();
        if (ret < 0) {
            LOGE("Softap Softap set - starting hostapd fails");
            return -1;
        }
    } else {
        struct ifreq ifr;
        char ifname[256];

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, argv[2], sizeof(ifr.ifr_name));

        /* Enable WLAN */
        ((int *)mBuf)[0] = AR6000_XIOCTRL_WMI_SET_WLAN_STATE;
        ((int *)mBuf)[1] = 1;
        ifr.ifr_data = mBuf;
        if ((ret = ioctl(mSock, AR6000_IOCTL_EXTENDED, &ifr)) < 0) {
            LOGE("AR6000_IOCTL %s set wlan state failed: %d:%s", ifr.ifr_name, ret, strerror(errno));
            return ret;
        }

        /* Add AP interface */
        if ((ret = if_rename(mSock, ifr.ifr_name, WIFI_AP_INTERFACE)) < 0) {
            LOGE("Softap startap - AR6000_IOCTL %s addif %s failed: %d", ifr.ifr_name, mIface, ret);
            return ret;
        }
        
        ret = wifi_start_hostapd();
        if (ret < 0) {
            LOGE("Softap startap - starting hostapd fails");
            return -1;
        }

        sched_yield();
        usleep(100000);
    }
	if (ret) {
        LOGE("Softap set - failed: %d", ret);
    }
    else {
        LOGD("Softap set - Ok");
        usleep(AP_SET_CFG_DELAY);
    }
#else
    strncpy(mIface, argv[3], sizeof(mIface));
    iface = argv[2];

#ifdef HAVE_HOSTAPD
    char *wbuf = NULL;
    char *fbuf = NULL;

    if (argc > 4) {
        ssid = argv[4];
    } else {
        ssid = (char *)"AndroidAP";
    }

    asprintf(&wbuf, "interface=%s\ndriver=nl80211\nctrl_interface="
            "/data/misc/wifi/hostapd\nssid=%s\nchannel=6\n", iface, ssid);

    if (argc > 5) {
        if (!strcmp(argv[5], "wpa-psk")) {
            generatePsk(ssid, argv[6], psk_str);
            asprintf(&fbuf, "%swpa=1\nwpa_pairwise=TKIP CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[5], "wpa2-psk")) {
            generatePsk(ssid, argv[6], psk_str);
            asprintf(&fbuf, "%swpa=2\nrsn_pairwise=CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[5], "open")) {
            asprintf(&fbuf, "%s", wbuf);
        }
    } else {
        asprintf(&fbuf, "%s", wbuf);
    }

    fd = open(HOSTAPD_CONF_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0660);
    if (fd < 0) {
        LOGE("Cannot update \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        free(wbuf);
        free(fbuf);
        return -1;
    }
    if (write(fd, fbuf, strlen(fbuf)) < 0) {
        LOGE("Cannot write to \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        ret = -1;
    }
    close(fd);
    free(wbuf);
    free(fbuf);

    /* Note: apparently open can fail to set permissions correctly at times */
    if (chmod(HOSTAPD_CONF_FILE, 0660) < 0) {
        LOGE("Error changing permissions of %s to 0660: %s",
                HOSTAPD_CONF_FILE, strerror(errno));
        unlink(HOSTAPD_CONF_FILE);
        return -1;
    }

    if (chown(HOSTAPD_CONF_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
                HOSTAPD_CONF_FILE, AID_WIFI, strerror(errno));
        unlink(HOSTAPD_CONF_FILE);
        return -1;
    }

#else
    /* Create command line */
    i = addParam(i, "ASCII_CMD", "AP_CFG");
    if (argc > 4) {
        ssid = argv[4];
    } else {
        ssid = (char *)"AndroidAP";
    }
    i = addParam(i, "SSID", ssid);
    if (argc > 5) {
        i = addParam(i, "SEC", argv[5]);
    } else {
        i = addParam(i, "SEC", "open");
    }
    if (argc > 6) {
        generatePsk(ssid, argv[6], psk_str);
        i = addParam(i, "KEY", psk_str);
    } else {
        i = addParam(i, "KEY", "12345678");
    }
    if (argc > 7) {
        i = addParam(i, "CHANNEL", argv[7]);
    } else {
        i = addParam(i, "CHANNEL", "6");
    }
    if (argc > 8) {
        i = addParam(i, "PREAMBLE", argv[8]);
    } else {
        i = addParam(i, "PREAMBLE", "0");
    }
    if (argc > 9) {
        i = addParam(i, "MAX_SCB", argv[9]);
    } else {
        i = addParam(i, "MAX_SCB", "8");
    }
    if ((i < 0) || ((unsigned)(i + 4) >= sizeof(mBuf))) {
        LOGE("Softap set - command is too big");
        return i;
    }
    sprintf(&mBuf[i], "END");

    /* system("iwpriv eth0 WL_AP_CFG ASCII_CMD=AP_CFG,SSID=\"AndroidAP\",SEC=\"open\",KEY=12345,CHANNEL=1,PREAMBLE=0,MAX_SCB=8,END"); */
    ret = setCommand(iface, "AP_SET_CFG");
    if (ret) {
        LOGE("Softap set - failed: %d", ret);
    }
    else {
        LOGD("Softap set - Ok");
        usleep(AP_SET_CFG_DELAY);
    }
#endif
#endif
    return ret;
}

void SoftapController::generatePsk(char *ssid, char *passphrase, char *psk_str) {
    unsigned char psk[SHA256_DIGEST_LENGTH];
    int j;
    // Use the PKCS#5 PBKDF2 with 4096 iterations
    PKCS5_PBKDF2_HMAC_SHA1(passphrase, strlen(passphrase),
            reinterpret_cast<const unsigned char *>(ssid), strlen(ssid),
            4096, SHA256_DIGEST_LENGTH, psk);
    for (j=0; j < SHA256_DIGEST_LENGTH; j++) {
        sprintf(&psk_str[j<<1], "%02x", psk[j]);
    }
    psk_str[j<<1] = '\0';
}


/*
 * Arguments:
 *	argv[2] - interface name
 *	argv[3] - AP or STA
 */
int SoftapController::fwReloadSoftap(int argc, char *argv[])
{
    int ret, i = 0;
    char *iface;
    char *fwpath;

    if (mSock < 0) {
        LOGE("Softap fwrealod - failed to open socket");
        return -1;
    }
    if (argc < 4) {
        LOGE("Softap fwreload - missing arguments");
        return -1;
    }
 LOGD("fwReloadSoftap: argv[2]:%s argv[3]:%s", argv[2], argv[3]);
#ifdef ATH_WIFI
    ret = 0;
#else
    iface = argv[2];

    if (strcmp(argv[3], "AP") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_AP);
    } else if (strcmp(argv[3], "P2P") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_P2P);
    } else {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_STA);
    }
    if (!fwpath)
        return -1;
#ifdef HAVE_HOSTAPD
    ret = wifi_change_fw_path((const char *)fwpath);
#else
    sprintf(mBuf, "FW_PATH=%s", fwpath);
    ret = setCommand(iface, "WL_FW_RELOAD");
#endif
#endif
    if (ret) {
        LOGE("Softap fwReload - failed: %d", ret);
    }
    else {
        LOGD("Softap fwReload - Ok");
    }
    return ret;
}

int SoftapController::clientsSoftap(char **retbuf)
{
    int ret;

    if (mSock < 0) {
        LOGE("Softap clients - failed to open socket");
        return -1;
    }
    *mBuf = 0;
    ret = setCommand(mIface, "AP_GET_STA_LIST", SOFTAP_MAX_BUFFER_SIZE);
    if (ret) {
        LOGE("Softap clients - failed: %d", ret);
    } else {
        asprintf(retbuf, "Softap clients:%s", mBuf);
        LOGD("Softap clients:%s", mBuf);
    }
    return ret;
}
