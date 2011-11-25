LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                                      \
                  BandwidthController.cpp              \
                  CommandListener.cpp                  \
                  DnsProxyListener.cpp                 \
                  NatController.cpp                    \
                  NetdCommand.cpp                      \
                  NetlinkHandler.cpp                   \
                  NetlinkManager.cpp                   \
                  PanController.cpp                    \
                  PppController.cpp                    \
                  ResolverController.cpp               \
                  SecondaryTableController.cpp         \
                  SoftapController.cpp                 \
                  TetherController.cpp                 \
                  ThrottleController.cpp               \
                  oem_iptables_hook.cpp                \
                  logwrapper.c                         \
                  main.cpp                             \


LOCAL_MODULE:= netd

LOCAL_C_INCLUDES := $(KERNEL_HEADERS) \
                    $(LOCAL_PATH)/../bluetooth/bluedroid/include \
                    $(LOCAL_PATH)/../bluetooth/bluez-clean-headers \
                    external/openssl/include \
                    external/stlport/stlport \
                    bionic \
                    $(call include-path-for, libhardware_legacy)/hardware_legacy

LOCAL_CFLAGS :=

LOCAL_SHARED_LIBRARIES := libstlport libsysutils libcutils libnetutils \
                          libcrypto libhardware_legacy

ifneq ($(BOARD_HOSTAPD_DRIVER),)
  LOCAL_CFLAGS += -DHAVE_HOSTAPD
endif

ifdef BOARD_WLAN_ATHEROS_SDK
LOCAL_CFLAGS += -I$(BOARD_WLAN_ATHEROS_SDK)/include
LOCAL_CFLAGS += -I$(BOARD_WLAN_ATHEROS_SDK)/host/include
LOCAL_CFLAGS += -I$(BOARD_WLAN_ATHEROS_SDK)/host/os/linux/include
LOCAL_CFLAGS += -I$(BOARD_WLAN_ATHEROS_SDK)/host/wlan/include
LOCAL_CFLAGS += -DATH_WIFI
LOCAL_STATIC_LIBRARIES := libhostapd_client
endif

ifeq ($(BOARD_HAVE_BLUETOOTH),true)
  LOCAL_SHARED_LIBRARIES := $(LOCAL_SHARED_LIBRARIES) libbluedroid
  LOCAL_CFLAGS := $(LOCAL_CFLAGS) -DHAVE_BLUETOOTH
endif

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_SRC_FILES:=          \
                  ndc.c \

LOCAL_MODULE:= ndc

LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

LOCAL_CFLAGS := 

LOCAL_SHARED_LIBRARIES := libcutils

include $(BUILD_EXECUTABLE)
