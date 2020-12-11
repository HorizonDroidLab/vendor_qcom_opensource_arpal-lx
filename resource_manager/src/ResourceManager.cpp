/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "PAL: ResourceManager"
#include "ResourceManager.h"
#include "Session.h"
#include "Device.h"
#include "Stream.h"
#include "StreamPCM.h"
#include "StreamCompress.h"
#include "StreamSoundTrigger.h"
#include "StreamInCall.h"
#include "gsl_intf.h"
#include "Headphone.h"
#include "PayloadBuilder.h"
#include "Bluetooth.h"
#include "SpeakerMic.h"
#include "Speaker.h"
#include "USBAudio.h"
#include "HeadsetMic.h"
#include "HandsetMic.h"
#include "DisplayPort.h"
#include "Handset.h"
#include "SndCardMonitor.h"
#include "agm_api.h"
#include <unistd.h>
#include <dlfcn.h>
#include <mutex>

#ifndef FEATURE_IPQ_OPENWRT
#include <cutils/str_parms.h>
#endif

#define XML_FILE_DELIMITER "_"
#define XML_FILE_EXT ".xml"
#define XML_PATH_MAX_LENGTH 100
#define HW_INFO_ARRAY_MAX_SIZE 32

#if defined(FEATURE_IPQ_OPENWRT) || defined(LINUX_ENABLED)
#define MIXER_XML_BASE_STRING "/etc/mixer_paths"
#define MIXER_XML_DEFAULT_PATH "/etc/mixer_paths_wsa.xml"
#define DEFAULT_ACDB_FILES "/etc/acdbdata/MTP/acdb_cal.acdb"
#define XMLFILE "/etc/resourcemanager.xml"
#define RMNGR_XMLFILE_BASE_STRING  "/etc/resourcemanager"
#define SNDPARSER "/etc/card-defs.xml"
#define STXMLFILE "/etc/sound_trigger_platform_info.xml"
#else
#define MIXER_XML_BASE_STRING "/vendor/etc/mixer_paths"
#define MIXER_XML_DEFAULT_PATH "/vendor/etc/mixer_paths_wsa.xml"
#define DEFAULT_ACDB_FILES "/vendor/etc/acdbdata/MTP/acdb_cal.acdb"
#define XMLFILE "/vendor/etc/resourcemanager.xml"
#define RMNGR_XMLFILE_BASE_STRING  "/vendor/etc/resourcemanager"
#define SNDPARSER "/vendor/etc/card-defs.xml"
#define STXMLFILE "/vendor/etc/sound_trigger_platform_info.xml"
#endif

#define MAX_SND_CARD 10
#define MAX_RETRY_CNT 20
#define LOWLATENCY_PCM_DEVICE 15
#define DEEP_BUFFER_PCM_DEVICE 0
#define DEVICE_NAME_MAX_SIZE 128

#define DEFAULT_BIT_WIDTH 16
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_FORMAT 0x00000000u
// TODO: double check and confirm actual
// values for max sessions number
#define MAX_SESSIONS_LOW_LATENCY 8
#define MAX_SESSIONS_ULTRA_LOW_LATENCY 8
#define MAX_SESSIONS_DEEP_BUFFER 3
#define MAX_SESSIONS_COMPRESSED 10
#define MAX_SESSIONS_GENERIC 1
#define MAX_SESSIONS_PCM_OFFLOAD 1
#define MAX_SESSIONS_VOICE_UI 8
#define MAX_SESSIONS_PROXY 8
#define DEFAULT_MAX_SESSIONS 8
#define DEFAULT_MAX_NT_SESSIONS 2
#define MAX_SESSIONS_INCALL_MUSIC 1
#define MAX_SESSIONS_INCALL_RECORD 1
#define MAX_SESSIONS_NON_TUNNEL 4

/*this can be over written by the config file settings*/
uint32_t pal_log_lvl = (PAL_LOG_ERR|PAL_LOG_INFO);

static struct str_parms *configParamKVPairs;

char rmngr_xml_file[XML_PATH_MAX_LENGTH] = {0};

struct snd_card_split {
    char device[HW_INFO_ARRAY_MAX_SIZE];
    char form_factor[HW_INFO_ARRAY_MAX_SIZE];
};

static struct snd_card_split cur_snd_card_split{
    .device = {0},
    .form_factor = {0},
};

// default properties which will be updated based on platform configuration
static struct pal_st_properties qst_properties = {
        "QUALCOMM Technologies, Inc",  // implementor
        "Sound Trigger HAL",  // description
        1,  // version
        { 0x68ab2d40, 0xe860, 0x11e3, 0x95ef,
         { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },  // uuid
        8,  // max_sound_models
        10,  // max_key_phrases
        10,  // max_users
        PAL_RECOGNITION_MODE_VOICE_TRIGGER |
        PAL_RECOGNITION_MODE_GENERIC_TRIGGER,  // recognition_modes
        true,  // capture_transition
        0,  // max_capture_ms
        false,  // concurrent_capture
        false,  // trigger_in_event
        0  // power_consumption_mw
};

/*
pcm device id is directly related to device,
using legacy design for alsa
*/
// Will update actual value when numbers got for VT

std::vector<std::pair<int32_t, std::string>> ResourceManager::deviceLinkName {
    {PAL_DEVICE_OUT_MIN,                  {std::string{ "none" }}},
    {PAL_DEVICE_NONE,                     {std::string{ "none" }}},
    {PAL_DEVICE_OUT_HANDSET,              {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPEAKER,              {std::string{ "" }}},
    {PAL_DEVICE_OUT_WIRED_HEADSET,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_WIRED_HEADPHONE,      {std::string{ "" }}},
    {PAL_DEVICE_OUT_LINE,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_SCO,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_A2DP,       {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_HDMI,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_DEVICE,           {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_HEADSET,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPDIF,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_FM,                   {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_LINE,             {std::string{ "" }}},
    {PAL_DEVICE_OUT_PROXY,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL_1,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_HEARING_AID,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_MAX,                  {std::string{ "none" }}},

    {PAL_DEVICE_IN_HANDSET_MIC,           {std::string{ "tdm-pri" }}},
    {PAL_DEVICE_IN_SPEAKER_MIC,           {std::string{ "tdm-pri" }}},
    {PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET, {std::string{ "" }}},
    {PAL_DEVICE_IN_WIRED_HEADSET,         {std::string{ "" }}},
    {PAL_DEVICE_IN_AUX_DIGITAL,           {std::string{ "" }}},
    {PAL_DEVICE_IN_HDMI,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_ACCESSORY,         {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_DEVICE,            {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_HEADSET,           {std::string{ "" }}},
    {PAL_DEVICE_IN_FM_TUNER,              {std::string{ "" }}},
    {PAL_DEVICE_IN_LINE,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_SPDIF,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_PROXY,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_HANDSET_VA_MIC,        {std::string{ "" }}},
    {PAL_DEVICE_IN_BLUETOOTH_A2DP,        {std::string{ "" }}},
    {PAL_DEVICE_IN_HEADSET_VA_MIC,        {std::string{ "" }}},
    {PAL_DEVICE_IN_TELEPHONY_RX,          {std::string{ "" }}},
};

std::vector<std::pair<int32_t, int32_t>> ResourceManager::devicePcmId {
    {PAL_DEVICE_OUT_MIN,                  0},
    {PAL_DEVICE_NONE,                     0},
    {PAL_DEVICE_OUT_HANDSET,              1},
    {PAL_DEVICE_OUT_SPEAKER,              1},
    {PAL_DEVICE_OUT_WIRED_HEADSET,        1},
    {PAL_DEVICE_OUT_WIRED_HEADPHONE,      1},
    {PAL_DEVICE_OUT_LINE,                 0},
    {PAL_DEVICE_OUT_BLUETOOTH_SCO,        0},
    {PAL_DEVICE_OUT_BLUETOOTH_A2DP,       0},
    {PAL_DEVICE_OUT_AUX_DIGITAL,          0},
    {PAL_DEVICE_OUT_HDMI,                 0},
    {PAL_DEVICE_OUT_USB_DEVICE,           0},
    {PAL_DEVICE_OUT_USB_HEADSET,          0},
    {PAL_DEVICE_OUT_SPDIF,                0},
    {PAL_DEVICE_OUT_FM,                   0},
    {PAL_DEVICE_OUT_AUX_LINE,             0},
    {PAL_DEVICE_OUT_PROXY,                0},
    {PAL_DEVICE_OUT_AUX_DIGITAL_1,        0},
    {PAL_DEVICE_OUT_HEARING_AID,          0},
    {PAL_DEVICE_OUT_MAX,                  0},

    {PAL_DEVICE_IN_HANDSET_MIC,           0},
    {PAL_DEVICE_IN_SPEAKER_MIC,           0},
    {PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET, 0},
    {PAL_DEVICE_IN_WIRED_HEADSET,         0},
    {PAL_DEVICE_IN_AUX_DIGITAL,           0},
    {PAL_DEVICE_IN_HDMI,                  0},
    {PAL_DEVICE_IN_USB_ACCESSORY,         0},
    {PAL_DEVICE_IN_USB_DEVICE,            0},
    {PAL_DEVICE_IN_USB_HEADSET,           0},
    {PAL_DEVICE_IN_FM_TUNER,              0},
    {PAL_DEVICE_IN_LINE,                  0},
    {PAL_DEVICE_IN_SPDIF,                 0},
    {PAL_DEVICE_IN_PROXY,                 0},
    {PAL_DEVICE_IN_HANDSET_VA_MIC,        0},
    {PAL_DEVICE_IN_BLUETOOTH_A2DP,        0},
    {PAL_DEVICE_IN_HEADSET_VA_MIC,        0},
    {PAL_DEVICE_IN_TELEPHONY_RX,          0},
};

// To be defined in detail
std::vector<std::pair<int32_t, std::string>> ResourceManager::sndDeviceNameLUT {
    {PAL_DEVICE_OUT_MIN,                  {std::string{ "" }}},
    {PAL_DEVICE_NONE,                     {std::string{ "none" }}},
    {PAL_DEVICE_OUT_HANDSET,              {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPEAKER,              {std::string{ "" }}},
    {PAL_DEVICE_OUT_WIRED_HEADSET,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_WIRED_HEADPHONE,      {std::string{ "" }}},
    {PAL_DEVICE_OUT_LINE,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_SCO,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_A2DP,       {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_HDMI,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_DEVICE,           {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_HEADSET,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPDIF,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_FM,                   {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_LINE,             {std::string{ "" }}},
    {PAL_DEVICE_OUT_PROXY,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL_1,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_HEARING_AID,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_MAX,                  {std::string{ "" }}},

    {PAL_DEVICE_IN_HANDSET_MIC,           {std::string{ "" }}},
    {PAL_DEVICE_IN_SPEAKER_MIC,           {std::string{ "" }}},
    {PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET, {std::string{ "" }}},
    {PAL_DEVICE_IN_WIRED_HEADSET,         {std::string{ "" }}},
    {PAL_DEVICE_IN_AUX_DIGITAL,           {std::string{ "" }}},
    {PAL_DEVICE_IN_HDMI,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_ACCESSORY,         {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_DEVICE,            {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_HEADSET,           {std::string{ "" }}},
    {PAL_DEVICE_IN_FM_TUNER,              {std::string{ "" }}},
    {PAL_DEVICE_IN_LINE,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_SPDIF,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_PROXY,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_HANDSET_VA_MIC,        {std::string{ "" }}},
    {PAL_DEVICE_IN_BLUETOOTH_A2DP,        {std::string{ "" }}},
    {PAL_DEVICE_IN_HEADSET_VA_MIC,        {std::string{ "" }}},
    {PAL_DEVICE_IN_VI_FEEDBACK,           {std::string{ "" }}},
    {PAL_DEVICE_IN_TELEPHONY_RX,          {std::string{ "" }}},
};

const std::map<std::string, uint32_t> usecaseIdLUT {
    {std::string{ "PAL_STREAM_LOW_LATENCY" },              PAL_STREAM_LOW_LATENCY},
    {std::string{ "PAL_STREAM_DEEP_BUFFER" },              PAL_STREAM_DEEP_BUFFER},
    {std::string{ "PAL_STREAM_COMPRESSED" },               PAL_STREAM_COMPRESSED},
    {std::string{ "PAL_STREAM_VOIP" },                     PAL_STREAM_VOIP},
    {std::string{ "PAL_STREAM_VOIP_RX" },                  PAL_STREAM_VOIP_RX},
    {std::string{ "PAL_STREAM_VOIP_TX" },                  PAL_STREAM_VOIP_TX},
    {std::string{ "PAL_STREAM_VOICE_CALL_MUSIC" },         PAL_STREAM_VOICE_CALL_MUSIC},
    {std::string{ "PAL_STREAM_GENERIC" },                  PAL_STREAM_GENERIC},
    {std::string{ "PAL_STREAM_RAW" },                      PAL_STREAM_RAW},
    {std::string{ "PAL_STREAM_VOICE_ACTIVATION" },         PAL_STREAM_VOICE_ACTIVATION},
    {std::string{ "PAL_STREAM_VOICE_CALL_RECORD" },        PAL_STREAM_VOICE_CALL_RECORD},
    {std::string{ "PAL_STREAM_VOICE_CALL_TX" },            PAL_STREAM_VOICE_CALL_TX},
    {std::string{ "PAL_STREAM_VOICE_CALL_RX_TX" },         PAL_STREAM_VOICE_CALL_RX_TX},
    {std::string{ "PAL_STREAM_VOICE_CALL" },               PAL_STREAM_VOICE_CALL},
    {std::string{ "PAL_STREAM_LOOPBACK" },                 PAL_STREAM_LOOPBACK},
    {std::string{ "PAL_STREAM_TRANSCODE" },                PAL_STREAM_TRANSCODE},
    {std::string{ "PAL_STREAM_VOICE_UI" },                 PAL_STREAM_VOICE_UI},
    {std::string{ "PAL_STREAM_PCM_OFFLOAD" },              PAL_STREAM_PCM_OFFLOAD},
    {std::string{ "PAL_STREAM_ULTRA_LOW_LATENCY" },        PAL_STREAM_ULTRA_LOW_LATENCY},
    {std::string{ "PAL_STREAM_PROXY" },                    PAL_STREAM_PROXY},
};

const std::map<std::string, sidetone_mode_t> sidetoneModetoId {
    {std::string{ "OFF" }, SIDETONE_OFF},
    {std::string{ "HW" },  SIDETONE_HW},
    {std::string{ "SW" },  SIDETONE_SW},
};

std::shared_ptr<ResourceManager> ResourceManager::rm = nullptr;
std::vector <int> ResourceManager::streamTag = {0};
std::vector <int> ResourceManager::streamPpTag = {0};
std::vector <int> ResourceManager::mixerTag = {0};
std::vector <int> ResourceManager::devicePpTag = {0};
std::vector <int> ResourceManager::deviceTag = {0};
std::mutex ResourceManager::mResourceManagerMutex;
std::mutex ResourceManager::mGraphMutex;
std::vector <int> ResourceManager::listAllFrontEndIds = {0};
std::vector <int> ResourceManager::listFreeFrontEndIds = {0};
std::vector <int> ResourceManager::listAllPcmPlaybackFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmRecordFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmLoopbackRxFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmLoopbackTxFrontEnds = {0};
std::vector <int> ResourceManager::listAllCompressPlaybackFrontEnds = {0};
std::vector <int> ResourceManager::listAllCompressRecordFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmVoice1RxFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmVoice1TxFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmVoice2RxFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmVoice2TxFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmInCallRecordFrontEnds = {0};
std::vector <int> ResourceManager::listAllPcmInCallMusicFrontEnds = {0};
std::vector <int> ResourceManager::listAllNonTunnelSessionIds = {0};
struct audio_mixer* ResourceManager::audio_mixer = NULL;
struct audio_route* ResourceManager::audio_route = NULL;
int ResourceManager::snd_card = 0;
std::vector<deviceCap> ResourceManager::devInfo;
static struct nativeAudioProp na_props;
SndCardMonitor* ResourceManager::sndmon = NULL;
std::mutex ResourceManager::cvMutex;
std::queue<card_status_t> ResourceManager::msgQ;
std::condition_variable ResourceManager::cv;
std::thread ResourceManager::workerThread;
std::thread ResourceManager::mixerEventTread;
bool ResourceManager::mixerClosed = false;
int ResourceManager::mixerEventRegisterCount = 0;
int ResourceManager::concurrencyEnableCount = 0;
int ResourceManager::concurrencyDisableCount = 0;
static int max_session_num;
bool ResourceManager::isSpeakerProtectionEnabled = false;
bool ResourceManager::isCpsEnabled = false;
int ResourceManager::bitWidthSupported = BITWIDTH_16;
static int max_nt_sessions;
bool ResourceManager::isRasEnabled = false;
bool ResourceManager::isMainSpeakerRight;
int ResourceManager::spQuickCalTime;
bool ResourceManager::isGaplessEnabled = false;
bool ResourceManager::isVIRecordStarted;

//TODO:Needs to define below APIs so that functionality won't break
#ifdef FEATURE_IPQ_OPENWRT
int str_parms_get_str(struct str_parms *str_parms, const char *key,
                      char *out_val, int len){return 0;}
char *str_parms_to_str(struct str_parms *str_parms){return NULL;}
int str_parms_add_str(struct str_parms *str_parms, const char *key,
                      const char *value){return 0;}
struct str_parms *str_parms_create(void){return NULL;}
void str_parms_del(struct str_parms *str_parms, const char *key){return;}
void str_parms_destroy(struct str_parms *str_parms){return;}

#endif

std::vector<deviceIn> ResourceManager::deviceInfo;
std::vector<tx_ecinfo> ResourceManager::txEcInfo;
struct vsid_info ResourceManager::vsidInfo;
std::vector<struct pal_amp_db_and_gain_table> ResourceManager::gainLvlMap;
std::map<std::pair<uint32_t, std::string>, std::string> ResourceManager::btCodecMap;

#define MAKE_STRING_FROM_ENUM(string) { {#string}, string }
std::map<std::string, uint32_t> ResourceManager::btFmtTable = {
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_AAC),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_SBC),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_APTX),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_APTX_HD),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_APTX_DUAL_MONO),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_LDAC),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_CELT),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_APTX_AD),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_APTX_AD_SPEECH),
    MAKE_STRING_FROM_ENUM(CODEC_TYPE_PCM)
};

std::vector<std::pair<int32_t, std::string>> ResourceManager::listAllBackEndIds {
    {PAL_DEVICE_OUT_MIN,                  {std::string{ "" }}},
    {PAL_DEVICE_NONE,                     {std::string{ "" }}},
    {PAL_DEVICE_OUT_HANDSET,              {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPEAKER,              {std::string{ "none" }}},
    {PAL_DEVICE_OUT_WIRED_HEADSET,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_WIRED_HEADPHONE,      {std::string{ "" }}},
    {PAL_DEVICE_OUT_LINE,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_SCO,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_BLUETOOTH_A2DP,       {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_HDMI,                 {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_DEVICE,           {std::string{ "" }}},
    {PAL_DEVICE_OUT_USB_HEADSET,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_SPDIF,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_FM,                   {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_LINE,             {std::string{ "" }}},
    {PAL_DEVICE_OUT_PROXY,                {std::string{ "" }}},
    {PAL_DEVICE_OUT_AUX_DIGITAL_1,        {std::string{ "" }}},
    {PAL_DEVICE_OUT_HEARING_AID,          {std::string{ "" }}},
    {PAL_DEVICE_OUT_MAX,                  {std::string{ "" }}},

    {PAL_DEVICE_IN_HANDSET_MIC,           {std::string{ "none" }}},
    {PAL_DEVICE_IN_SPEAKER_MIC,           {std::string{ "none" }}},
    {PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET, {std::string{ "" }}},
    {PAL_DEVICE_IN_WIRED_HEADSET,         {std::string{ "" }}},
    {PAL_DEVICE_IN_AUX_DIGITAL,           {std::string{ "" }}},
    {PAL_DEVICE_IN_HDMI,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_ACCESSORY,         {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_DEVICE,            {std::string{ "" }}},
    {PAL_DEVICE_IN_USB_HEADSET,           {std::string{ "" }}},
    {PAL_DEVICE_IN_FM_TUNER,              {std::string{ "" }}},
    {PAL_DEVICE_IN_LINE,                  {std::string{ "" }}},
    {PAL_DEVICE_IN_SPDIF,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_PROXY,                 {std::string{ "" }}},
    {PAL_DEVICE_IN_HANDSET_VA_MIC,        {std::string{ "none" }}},
    {PAL_DEVICE_IN_BLUETOOTH_A2DP,        {std::string{ "" }}},
    {PAL_DEVICE_IN_HEADSET_VA_MIC,        {std::string{ "none" }}},
    {PAL_DEVICE_IN_VI_FEEDBACK,           {std::string{ "" }}},
    {PAL_DEVICE_IN_TELEPHONY_RX,          {std::string{ "" }}},
};

void agmServiceCrashHandler(uint64_t cookie __unused)
{
    PAL_ERR(LOG_TAG,"AGM service crashed :( ");
    _exit(1);
}

void ResourceManager::split_snd_card(const char* in_snd_card_name)
{
    /* Sound card name follows below mentioned convention:
       <target name>-<form factor>-snd-card.
       Parse target name and form factor.
    */
    char *snd_card_name = NULL;
    char *tmp = NULL;
    char *device = NULL;
    char *form_factor = NULL;

    if (in_snd_card_name == NULL) {
        PAL_ERR(LOG_TAG,"snd_card_name passed is NULL");
        goto err;
    }
    snd_card_name = strdup(in_snd_card_name);

    device = strtok_r(snd_card_name, "-", &tmp);
    if (device == NULL) {
        PAL_ERR(LOG_TAG,"called on invalid snd card name");
        goto err;
    }
    strlcpy(cur_snd_card_split.device, device, HW_INFO_ARRAY_MAX_SIZE);

    form_factor = strtok_r(NULL, "-", &tmp);
    if (form_factor == NULL) {
        PAL_ERR(LOG_TAG, "called on invalid snd card name");
        goto err;
    }
    strlcpy(cur_snd_card_split.form_factor, form_factor, HW_INFO_ARRAY_MAX_SIZE);

    PAL_INFO(LOG_TAG, "snd_card_name(%s) device(%s) form_factor(%s)",
             in_snd_card_name, device, form_factor);

err:
    if (snd_card_name)
        free(snd_card_name);
}

ResourceManager::ResourceManager()
{
    int ret = 0;
    // Init audio_route and audio_mixer

    na_props.rm_na_prop_enabled = false;
    na_props.ui_na_prop_enabled = false;
    na_props.na_mode = NATIVE_AUDIO_MODE_INVALID;

    max_session_num = DEFAULT_MAX_SESSIONS;
    max_nt_sessions = DEFAULT_MAX_NT_SESSIONS;
    //TODO: parse the tag and populate in the tags
    streamTag.clear();
    deviceTag.clear();
    btCodecMap.clear();

    ret = ResourceManager::init_audio();
    if (ret) {
        PAL_ERR(LOG_TAG, "error in init audio route and audio mixer ret %d", ret);
    }

    ret = ResourceManager::XmlParser(rmngr_xml_file);
    if (ret) {
        PAL_ERR(LOG_TAG, "error in resource xml parsing ret %d", ret);
    }

    listAllFrontEndIds.clear();
    listFreeFrontEndIds.clear();
    listAllPcmPlaybackFrontEnds.clear();
    listAllPcmRecordFrontEnds.clear();
    listAllPcmLoopbackRxFrontEnds.clear();
    listAllNonTunnelSessionIds.clear();
    listAllPcmLoopbackTxFrontEnds.clear();
    listAllCompressPlaybackFrontEnds.clear();
    listAllCompressRecordFrontEnds.clear();
    listAllPcmVoice1RxFrontEnds.clear();
    listAllPcmVoice1TxFrontEnds.clear();
    listAllPcmVoice2RxFrontEnds.clear();
    listAllPcmVoice2TxFrontEnds.clear();
    listAllPcmInCallRecordFrontEnds.clear();
    listAllPcmInCallMusicFrontEnds.clear();
    memset(stream_instances, 0, PAL_STREAM_MAX * sizeof(uint64_t));

    ret = ResourceManager::XmlParser(SNDPARSER);
    if (ret) {
        PAL_ERR(LOG_TAG, "error in snd xml parsing ret %d", ret);
    }
    for (int i=0; i < devInfo.size(); i++) {

        if (devInfo[i].type == PCM) {
            if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].playback == 1) {
                listAllPcmLoopbackRxFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].record == 1) {
                listAllPcmLoopbackTxFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].playback == 1 && devInfo[i].sess_mode == DEFAULT) {
                listAllPcmPlaybackFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].record == 1 && devInfo[i].sess_mode == DEFAULT) {
                listAllPcmRecordFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].sess_mode == NON_TUNNEL && devInfo[i].record == 1) {
                listAllPcmInCallRecordFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].sess_mode == NON_TUNNEL && devInfo[i].playback == 1) {
                listAllPcmInCallMusicFrontEnds.push_back(devInfo[i].deviceId);
            }
        } else if (devInfo[i].type == COMPRESS) {
            if (devInfo[i].playback == 1) {
                listAllCompressPlaybackFrontEnds.push_back(devInfo[i].deviceId);
            } else if (devInfo[i].record == 1) {
                listAllCompressRecordFrontEnds.push_back(devInfo[i].deviceId);
            }
        } else if (devInfo[i].type == VOICE1) {
            if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].playback == 1) {
                listAllPcmVoice1RxFrontEnds.push_back(devInfo[i].deviceId);
            }
            if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].record == 1) {
                listAllPcmVoice1TxFrontEnds.push_back(devInfo[i].deviceId);
            }
        } else if (devInfo[i].type == VOICE2) {
            if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].playback == 1) {
                listAllPcmVoice2RxFrontEnds.push_back(devInfo[i].deviceId);
            }
            if (devInfo[i].sess_mode == HOSTLESS && devInfo[i].record == 1) {
                listAllPcmVoice2TxFrontEnds.push_back(devInfo[i].deviceId);
            }
        }
        /*We create a master list of all the frontends*/
        listAllFrontEndIds.push_back(devInfo[i].deviceId);
    }
    /*
     *Arrange all the FrontendIds in descending order, this gives the
     *largest deviceId being used for ALSA usecases.
     *For NON-TUNNEL usecases the sessionIds to be used are formed by incrementing the largest used deviceID
     *with number of non-tunnel sessions supported on a platform. This way we avoid any conflict of deviceIDs.
     */
     sort(listAllFrontEndIds.rbegin(), listAllFrontEndIds.rend());
     int maxDeviceIdInUse = listAllFrontEndIds.at(0);
     for (int i = 0; i < max_nt_sessions; i++)
          listAllNonTunnelSessionIds.push_back(maxDeviceIdInUse + i);

    // Get AGM service handle
    ret = agm_register_service_crash_callback(&agmServiceCrashHandler,
                                               (uint64_t)this);
    if (ret) {
        PAL_ERR(LOG_TAG, "AGM service not up%d", ret);
    }

    ResourceManager::loadAdmLib();

}

ResourceManager::~ResourceManager()
{
    streamTag.clear();
    streamPpTag.clear();
    mixerTag.clear();
    devicePpTag.clear();
    deviceTag.clear();

    listAllFrontEndIds.clear();
    listAllPcmPlaybackFrontEnds.clear();
    listAllPcmRecordFrontEnds.clear();
    listAllPcmLoopbackRxFrontEnds.clear();
    listAllPcmLoopbackTxFrontEnds.clear();
    listAllCompressPlaybackFrontEnds.clear();
    listAllCompressRecordFrontEnds.clear();
    listFreeFrontEndIds.clear();
    listAllPcmVoice1RxFrontEnds.clear();
    listAllPcmVoice1TxFrontEnds.clear();
    listAllPcmVoice2RxFrontEnds.clear();
    listAllPcmVoice2TxFrontEnds.clear();
    listAllNonTunnelSessionIds.clear();
    devInfo.clear();
    deviceInfo.clear();
    txEcInfo.clear();

    STInstancesLists.clear();
    listAllBackEndIds.clear();
    sndDeviceNameLUT.clear();
    devicePcmId.clear();
    deviceLinkName.clear();

    if (admLibHdl) {
        if (admDeInitFn)
            admDeInitFn(admData);
        dlclose(admLibHdl);
    }

}

void ResourceManager::loadAdmLib()
{
    if (access(ADM_LIBRARY_PATH, R_OK) == 0) {
        admLibHdl = dlopen(ADM_LIBRARY_PATH, RTLD_NOW);
        if (admLibHdl == NULL) {
            PAL_INFO(LOG_TAG, "DLOPEN failed for %s", ADM_LIBRARY_PATH);
        } else {
            PAL_INFO(LOG_TAG, "DLOPEN successful for %s", ADM_LIBRARY_PATH);
            admInitFn = (adm_init_t)
                dlsym(admLibHdl, "adm_init");
            admDeInitFn = (adm_deinit_t)
                dlsym(admLibHdl, "adm_deinit");
            admRegisterInputStreamFn = (adm_register_input_stream_t)
                dlsym(admLibHdl, "adm_register_input_stream");
            admRegisterOutputStreamFn = (adm_register_output_stream_t)
                dlsym(admLibHdl, "adm_register_output_stream");
            admDeregisterStreamFn = (adm_deregister_stream_t)
                dlsym(admLibHdl, "adm_deregister_stream");
            admRequestFocusFn = (adm_request_focus_t)
                dlsym(admLibHdl, "adm_request_focus");
            admAbandonFocusFn = (adm_abandon_focus_t)
                dlsym(admLibHdl, "adm_abandon_focus");
            admSetConfigFn = (adm_set_config_t)
                dlsym(admLibHdl, "adm_set_config");
            admRequestFocusV2Fn = (adm_request_focus_v2_t)
                dlsym(admLibHdl, "adm_request_focus_v2");
            admOnRoutingChangeFn = (adm_on_routing_change_t)
                dlsym(admLibHdl, "adm_on_routing_change");
            admRequestFocus_v2_1Fn = (adm_request_focus_v2_1_t)
                dlsym(admLibHdl, "adm_request_focus_v2_1");


            if (admInitFn)
                admData = admInitFn();
        }
    }
}

void ResourceManager::ssrHandlingLoop(std::shared_ptr<ResourceManager> rm)
{
    card_status_t state;
    card_status_t prevState = CARD_STATUS_ONLINE;
    std::unique_lock<std::mutex> lock(rm->cvMutex);
    int ret = 0;
    uint32_t eventData;
    pal_global_callback_event_t event;

    PAL_INFO(LOG_TAG,"ssr Handling thread started");

    while(1) {
        if (rm->msgQ.empty())
            rm->cv.wait(lock);
        if (!rm->msgQ.empty()) {
            state = rm->msgQ.front();
            rm->msgQ.pop();
            lock.unlock();
            PAL_INFO(LOG_TAG, "state %d, prev state %d size %zu",
                               state, prevState, rm->mActiveStreams.size());
            if (state == CARD_STATUS_NONE)
                break;

            rm->cardState = state;
            mResourceManagerMutex.lock();
            if (state != prevState) {
                if (rm->globalCb) {
                    PAL_DBG(LOG_TAG, "Notifying client about sound card state %d global cb %pK",
                                      rm->cardState, rm->globalCb);
                    eventData = (int)rm->cardState;
                    event = PAL_SND_CARD_STATE;
                    PAL_DBG(LOG_TAG, "eventdata %d", eventData);
                    rm->globalCb(event, &eventData, cookie);
                }
            }

            if (rm->mActiveStreams.empty()) {
                PAL_INFO(LOG_TAG, "Idle SSR : No streams registered yet.");
                prevState = state;
            } else if (state == prevState) {
                PAL_INFO(LOG_TAG, "%d state already handled", state);
            } else if (state == CARD_STATUS_OFFLINE) {
                for (auto str: rm->mActiveStreams) {
                    auto iter = std::find(mActiveStreams.begin(), mActiveStreams.end(), str);
                    str->ssrDone = false;
                    mResourceManagerMutex.unlock();
                    if (iter != mActiveStreams.end()) {
                        ret = str->ssrDownHandler();
                        if (0 != ret) {
                            PAL_ERR(LOG_TAG, "Ssr down handling failed for %pK ret %d",
                                          str, ret);
                        }
                    }
                    mResourceManagerMutex.lock();
                }
                prevState = state;
            } else if (state == CARD_STATUS_ONLINE) {
                for (auto str: rm->mActiveStreams) {
                    auto iter = std::find(mActiveStreams.begin(), mActiveStreams.end(), str);
                    str->ssrDone = false;
                    mResourceManagerMutex.unlock();
                    if (iter != mActiveStreams.end()) {
                        ret = str->ssrUpHandler();
                        if (0 != ret) {
                            PAL_ERR(LOG_TAG, "Ssr up handling failed for %pK ret %d",
                                          str, ret);
                        }
                    }
                    mResourceManagerMutex.lock();
                }
                prevState = state;
            } else {
                PAL_ERR(LOG_TAG, "Invalid state. state %d", state);
            }
            mResourceManagerMutex.unlock();
            lock.lock();
        }
    }
    PAL_INFO(LOG_TAG, "ssr Handling thread ended");
}

int ResourceManager::initSndMonitor()
{
    int ret = 0;
    workerThread = std::thread(&ResourceManager::ssrHandlingLoop, this, rm);
    sndmon = new SndCardMonitor(snd_card);
    if (!sndmon) {
        ret = -EINVAL;
        PAL_ERR(LOG_TAG, "Sound monitor creation failed, ret %d", ret);
        return ret;
    } else {
        cardState = CARD_STATUS_ONLINE;
        PAL_INFO(LOG_TAG, "Sound monitor initialized");
        return ret;
    }
}

void ResourceManager::ssrHandler(card_status_t state)
{
    PAL_DBG(LOG_TAG, "Enter. state %d", state);
    cvMutex.lock();
    msgQ.push(state);
    cvMutex.unlock();
    cv.notify_all();
    return;
}

char* ResourceManager::getDeviceNameFromID(uint32_t id)
{
    for (int i=0; i < devInfo.size(); i++) {
        if (devInfo[i].deviceId == id) {
            PAL_DBG(LOG_TAG, "pcm id name is %s ", devInfo[i].name);
            return devInfo[i].name;
        }
    }

    return NULL;
}

int ResourceManager::init_audio()
{
    int retry = 0;
    bool snd_card_found = false;

    char *snd_card_name = NULL;

    char mixer_xml_file[XML_PATH_MAX_LENGTH] = {0};

    PAL_DBG(LOG_TAG, "Enter.");

    do {
        /* Look for only default codec sound card */
        /* Ignore USB sound card if detected */
        snd_card = 0;
        while (snd_card < MAX_SND_CARD) {
            struct audio_mixer* tmp_mixer = NULL;
            tmp_mixer = mixer_open(snd_card);
            if (tmp_mixer) {
                snd_card_name = strdup(mixer_get_name(tmp_mixer));
                if (!snd_card_name) {
                    PAL_ERR(LOG_TAG, "failed to allocate memory for snd_card_name");
                    mixer_close(tmp_mixer);
                    return -EINVAL;
                }
                PAL_INFO(LOG_TAG, "mixer_open success. snd_card_num = %d, snd_card_name %s, am:%p",
                snd_card, snd_card_name, rm->audio_mixer);

                /* TODO: Needs to extend for new targets */
                if (strstr(snd_card_name, "kona") ||
                    strstr(snd_card_name, "sm8150")||
                    strstr(snd_card_name, "lahaina") ) {
                    PAL_VERBOSE(LOG_TAG, "Found Codec sound card");
                    snd_card_found = true;
                    audio_mixer = tmp_mixer;
                    break;
                } else {
                    if (snd_card_name) {
                        free(snd_card_name);
                        snd_card_name = NULL;
                    }
                    mixer_close(tmp_mixer);
                }
            }
            snd_card++;
        }

        if (!snd_card_found) {
            PAL_INFO(LOG_TAG, "No audio mixer, retry %d", retry++);
            sleep(1);
        }
    } while (!snd_card_found && retry <= MAX_RETRY_CNT);

    if (snd_card >= MAX_SND_CARD || !audio_mixer) {
        PAL_ERR(LOG_TAG, "audio mixer open failure");
        return -EINVAL;
    }

    split_snd_card(snd_card_name);

    strlcpy(mixer_xml_file, MIXER_XML_BASE_STRING, XML_PATH_MAX_LENGTH);
    strlcpy(rmngr_xml_file, RMNGR_XMLFILE_BASE_STRING, XML_PATH_MAX_LENGTH);
    /* Note: This assumes IDP/MTP form factor will use mixer_paths.xml /
             resourcemanager.xml.
       TODO: Add support for form factors other than IDP/QRD.
    */
    if (!strncmp(cur_snd_card_split.form_factor, "qrd", sizeof ("qrd"))||
        !strncmp(cur_snd_card_split.form_factor, "cdp", sizeof ("cdp"))){
            strlcat(mixer_xml_file, XML_FILE_DELIMITER, XML_PATH_MAX_LENGTH);
            strlcat(mixer_xml_file, cur_snd_card_split.form_factor, XML_PATH_MAX_LENGTH);

            strlcat(rmngr_xml_file, XML_FILE_DELIMITER, XML_PATH_MAX_LENGTH);
            strlcat(rmngr_xml_file, cur_snd_card_split.form_factor, XML_PATH_MAX_LENGTH);
    }
    strlcat(mixer_xml_file, XML_FILE_EXT, XML_PATH_MAX_LENGTH);
    strlcat(rmngr_xml_file, XML_FILE_EXT, XML_PATH_MAX_LENGTH);

    audio_route = audio_route_init(snd_card, mixer_xml_file);
    PAL_INFO(LOG_TAG, "audio route %pK, mixer path %s", audio_route, mixer_xml_file);
    if (!audio_route) {
        PAL_ERR(LOG_TAG, "audio route init failed");
        mixer_close(audio_mixer);
        if (snd_card_name)
            free(snd_card_name);
        return -EINVAL;
    }
    // audio_route init success

    PAL_DBG(LOG_TAG, "Exit. audio route init success with card %d mixer path %s",
            snd_card, mixer_xml_file);
    return 0;
}

int ResourceManager::init()
{
    std::shared_ptr<Speaker> dev = nullptr;

    // Initialize Speaker Protection calibration mode
    struct pal_device dattr;

    mixerEventTread = std::thread(mixerEventWaitThreadLoop, rm);

    // Get the speaker instance and activate speaker protection
    dattr.id = PAL_DEVICE_OUT_SPEAKER;
    dev = std::dynamic_pointer_cast<Speaker>(Device::getInstance(&dattr , rm));
    if (dev) {
        PAL_DBG(LOG_TAG, "Speaker instance created");
    }
    else
        PAL_INFO(LOG_TAG, "Speaker instance not created");

    return 0;
}

bool ResourceManager::getEcRefStatus(pal_stream_type_t tx_streamtype,pal_stream_type_t rx_streamtype)
{
    bool ecref_status = true;
    if (tx_streamtype == PAL_STREAM_LOW_LATENCY) {
       PAL_DBG(LOG_TAG, "no need to enable ec for tx stream %d", tx_streamtype);
       return false;
    }
    for (int i = 0; i < txEcInfo.size(); i++) {
        if (tx_streamtype == txEcInfo[i].tx_stream_type) {
            for (auto rx_type = txEcInfo[i].disabled_rx_streams.begin();
                  rx_type != txEcInfo[i].disabled_rx_streams.end(); rx_type++) {
               if (rx_streamtype == *rx_type) {
                   ecref_status = false;
                   PAL_DBG(LOG_TAG, "given rx %d disabled %d status %d",rx_streamtype, *rx_type, ecref_status);
                   break;
               }
            }
        }
    }
    return ecref_status;
}

void ResourceManager::getDeviceInfo(pal_device_id_t deviceId, pal_stream_type_t type,
                                         struct pal_device_info *devinfo)
{
    struct kvpair_info kv = {};

    for (int32_t size1 = 0; size1 < deviceInfo.size(); size1++) {
        if (deviceId == deviceInfo[size1].deviceId) {
            devinfo->channels = deviceInfo[size1].channel;
            devinfo->max_channels = deviceInfo[size1].max_channel;
            for (int32_t size2 = 0; size2 < deviceInfo[size1].usecase.size(); size2++) {
                if (type == deviceInfo[size1].usecase[size2].type) {
                    for (int32_t kvsize = 0;
                    kvsize < deviceInfo[size1].usecase[size2].kvpair.size(); kvsize++) {
                       kv.key =  deviceInfo[size1].usecase[size2].kvpair[kvsize].key;
                       kv.value =  deviceInfo[size1].usecase[size2].kvpair[kvsize].value;
                       devinfo->kvpair.push_back(kv);
                    }
                    return;
               }
            }
        }
     }
}

int32_t ResourceManager::getSidetoneMode(pal_device_id_t deviceId,
                                         pal_stream_type_t type,
                                         sidetone_mode_t *mode){
    int32_t status = 0;

    *mode = SIDETONE_OFF;
    for (int32_t size1 = 0; size1 < deviceInfo.size(); size1++) {
        if (deviceId == deviceInfo[size1].deviceId) {
            for (int32_t size2 = 0; size2 < deviceInfo[size1].usecase.size(); size2++) {
                if (type == deviceInfo[size1].usecase[size2].type) {
                    *mode = deviceInfo[size1].usecase[size2].sidetoneMode;
                    PAL_DBG(LOG_TAG, "found sidetoneMode %d for dev %d", *mode, deviceId);
                    break;
                }
            }
        }
    }
    return status;
}

int32_t ResourceManager::getVsidInfo(struct vsid_info  *info) {
    int status = 0;
    struct vsid_modepair modePair = {};

    info->vsid = vsidInfo.vsid;
    for (int size = 0; size < vsidInfo.modepair.size(); size++) {
        modePair.key = vsidInfo.modepair[size].key;
        modePair.value = vsidInfo.modepair[size].value;
        info->modepair.push_back(modePair);
    }
    return status;

}

void ResourceManager::getChannelMap(uint8_t *channel_map, int channels)
{
    switch (channels) {
    case CHANNELS_1:
       channel_map[0] = PAL_CHMAP_CHANNEL_C;
       break;
    case CHANNELS_2:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       break;
    case CHANNELS_3:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_C;
       break;
    case CHANNELS_4:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_LB;
       channel_map[3] = PAL_CHMAP_CHANNEL_RB;
       break;
    case CHANNELS_5:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_C;
       channel_map[3] = PAL_CHMAP_CHANNEL_LB;
       channel_map[4] = PAL_CHMAP_CHANNEL_RB;
       break;
    case CHANNELS_6:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_C;
       channel_map[3] = PAL_CHMAP_CHANNEL_LFE;
       channel_map[4] = PAL_CHMAP_CHANNEL_LB;
       channel_map[5] = PAL_CHMAP_CHANNEL_RB;
       break;
    case CHANNELS_7:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_C;
       channel_map[3] = PAL_CHMAP_CHANNEL_LFE;
       channel_map[4] = PAL_CHMAP_CHANNEL_LB;
       channel_map[5] = PAL_CHMAP_CHANNEL_RB;
       channel_map[6] = PAL_CHMAP_CHANNEL_RC;
       break;
    case CHANNELS_8:
       channel_map[0] = PAL_CHMAP_CHANNEL_FL;
       channel_map[1] = PAL_CHMAP_CHANNEL_FR;
       channel_map[2] = PAL_CHMAP_CHANNEL_C;
       channel_map[3] = PAL_CHMAP_CHANNEL_LFE;
       channel_map[4] = PAL_CHMAP_CHANNEL_LB;
       channel_map[5] = PAL_CHMAP_CHANNEL_RB;
       channel_map[6] = PAL_CHMAP_CHANNEL_LS;
       channel_map[7] = PAL_CHMAP_CHANNEL_RS;
       break;
   }
}

int32_t ResourceManager::getDeviceConfig(struct pal_device *deviceattr,
                                         struct pal_stream_attributes *sAttr, int32_t channel)
{
    int32_t status = 0;
    struct pal_channel_info dev_ch_info;
    bool is_wfd_in_progress = false;
    struct pal_stream_attributes tx_attr;

    PAL_DBG(LOG_TAG, "deviceattr->id %d", deviceattr->id);
    switch (deviceattr->id) {
        case PAL_DEVICE_IN_SPEAKER_MIC:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            PAL_DBG(LOG_TAG, "deviceattr->config.ch_info.channels %d", deviceattr->config.ch_info.channels);
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_IN_HANDSET_MIC:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            PAL_DBG(LOG_TAG, "deviceattr->config.ch_info.channels %d", deviceattr->config.ch_info.channels);
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_IN_WIRED_HEADSET:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            PAL_DBG(LOG_TAG, "deviceattr->config.ch_info.channels %d", deviceattr->config.ch_info.channels);
            deviceattr->config.sample_rate = sAttr->in_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->in_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            status = (HeadsetMic::checkAndUpdateBitWidth(&deviceattr->config.bit_width) |
                HeadsetMic::checkAndUpdateSampleRate(&deviceattr->config.sample_rate));
            if (status) {
                PAL_ERR(LOG_TAG, "failed to update samplerate/bitwidth");
                status = -EINVAL;
            }
            PAL_DBG(LOG_TAG, "device samplerate %d, bitwidth %d", deviceattr->config.sample_rate, deviceattr->config.bit_width);
            break;
        case PAL_DEVICE_OUT_HANDSET:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            PAL_DBG(LOG_TAG, "deviceattr->config.ch_info.channels %d", deviceattr->config.ch_info.channels);
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_OUT_SPEAKER:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = bitWidthSupported;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_IN_VI_FEEDBACK:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_32;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_OUT_WIRED_HEADPHONE:
        case PAL_DEVICE_OUT_WIRED_HEADSET:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            deviceattr->config.sample_rate = sAttr->out_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->out_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            status = (Headphone::checkAndUpdateBitWidth(&deviceattr->config.bit_width) |
                Headphone::checkAndUpdateSampleRate(&deviceattr->config.sample_rate));
            if (status) {
                PAL_ERR(LOG_TAG, "failed to update samplerate/bitwidth");
                status = -EINVAL;
            }
            PAL_DBG(LOG_TAG, "device samplerate %d, bitwidth %d", deviceattr->config.sample_rate, deviceattr->config.bit_width);
            break;
        case PAL_DEVICE_IN_HANDSET_VA_MIC:
        case PAL_DEVICE_IN_HEADSET_VA_MIC:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            PAL_DBG(LOG_TAG, "deviceattr->config.ch_info.channels %d", deviceattr->config.ch_info.channels);
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
        case PAL_DEVICE_OUT_BLUETOOTH_A2DP:
        case PAL_DEVICE_IN_BLUETOOTH_A2DP:
            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            deviceattr->config.sample_rate = SAMPLINGRATE_44K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_COMPRESSED;
            break;
        case PAL_DEVICE_OUT_BLUETOOTH_SCO:
        case PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET:
        {
            std::shared_ptr<BtSco> scoDev;

            dev_ch_info.channels = channel;
            getChannelMap(&(dev_ch_info.ch_map[0]), channel);
            deviceattr->config.ch_info = dev_ch_info;
            deviceattr->config.sample_rate = SAMPLINGRATE_8K;  /* Updated when WBS set param is received */
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            scoDev = std::dynamic_pointer_cast<BtSco>(BtSco::getInstance(deviceattr, rm));
            if (!scoDev) {
                PAL_ERR(LOG_TAG, "failed to get BtSco singleton object.");
                return -EINVAL;
            }
            scoDev->updateSampleRate(&deviceattr->config.sample_rate);
            PAL_DBG(LOG_TAG, "BT SCO device samplerate %d, bitwidth %d",
                  deviceattr->config.sample_rate, deviceattr->config.bit_width);
        }
            break;
        case PAL_DEVICE_OUT_USB_DEVICE:
        case PAL_DEVICE_OUT_USB_HEADSET:
            {
                deviceattr->config.sample_rate = SAMPLINGRATE_44K;//SAMPLINGRATE_48K;
                deviceattr->config.bit_width = BITWIDTH_16;
                deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
                // config.ch_info memory is allocated in selectBestConfig below
                std::shared_ptr<USB> USB_out_device;
                USB_out_device = std::dynamic_pointer_cast<USB>(USB::getInstance(deviceattr, rm));
                if (!USB_out_device) {
                    PAL_ERR(LOG_TAG, "failed to get USB singleton object.");
                    return -EINVAL;
                }
                status = USB_out_device->selectBestConfig(deviceattr, sAttr, true);
                PAL_ERR(LOG_TAG, "device samplerate %d, bitwidth %d, ch %d", deviceattr->config.sample_rate, deviceattr->config.bit_width,
                        deviceattr->config.ch_info.channels);
            }
            break;
        case PAL_DEVICE_IN_USB_DEVICE:
        case PAL_DEVICE_IN_USB_HEADSET:
            {
            deviceattr->config.sample_rate = SAMPLINGRATE_48K;
            deviceattr->config.bit_width = BITWIDTH_16;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            std::shared_ptr<USB> USB_in_device;
            USB_in_device = std::dynamic_pointer_cast<USB>(USB::getInstance(deviceattr, rm));
            if (!USB_in_device) {
                PAL_ERR(LOG_TAG, "failed to get USB singleton object.");
                return -EINVAL;
            }
            USB_in_device->selectBestConfig(deviceattr, sAttr, false);
            }
            break;
        case PAL_DEVICE_IN_PROXY:
            {
            /* For PAL_DEVICE_IN_PROXY, copy all config from stream attributes */
            deviceattr->config.ch_info =sAttr->in_media_config.ch_info;
            deviceattr->config.sample_rate = sAttr->in_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->in_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;

            PAL_DBG(LOG_TAG, "PAL_DEVICE_IN_PROXY sample rate %d bitwidth %d",
                    deviceattr->config.sample_rate, deviceattr->config.bit_width);
            }
            break;
        case PAL_DEVICE_IN_FM_TUNER:
            {
            /* For PAL_DEVICE_IN_FM_TUNER, copy all config from stream attributes */
            deviceattr->config.ch_info = sAttr->in_media_config.ch_info;
            deviceattr->config.sample_rate = sAttr->in_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->in_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            PAL_DBG(LOG_TAG, "PAL_DEVICE_IN_FM_TUNER sample rate %d bitwidth %d",
                    deviceattr->config.sample_rate, deviceattr->config.bit_width);
            }
            break;
        case PAL_DEVICE_OUT_PROXY:
            {
            // check if wfd session in progress
            for (auto& tx_str: mActiveStreams) {
                tx_str->getStreamAttributes(&tx_attr);
                if (tx_attr.direction == PAL_AUDIO_INPUT &&
                    tx_attr.info.opt_stream_info.tx_proxy_type == PAL_STREAM_PROXY_TX_WFD) {
                    is_wfd_in_progress = true;
                    break;
                }
            }
            if (is_wfd_in_progress)
            {
                std::shared_ptr<Device> dev = nullptr;
                struct pal_device proxyIn_dattr;
                proxyIn_dattr.id = PAL_DEVICE_IN_PROXY;
                dev = Device::getInstance(&proxyIn_dattr, rm);
                if (dev) {
                    status = dev->getDeviceAttributes(&proxyIn_dattr);
                    if (status) {
                        PAL_ERR(LOG_TAG, "OUT_PROXY getDeviceAttributes failed %d", status);
                        break;
                    }
                    deviceattr->config.ch_info = proxyIn_dattr.config.ch_info;
                    deviceattr->config.sample_rate = proxyIn_dattr.config.sample_rate;
                    deviceattr->config.bit_width = proxyIn_dattr.config.bit_width;
                    deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
                    PAL_DBG(LOG_TAG, "PAL_DEVICE_OUT_PROXY samplereate %d bitwidth %d",
                            deviceattr->config.sample_rate, deviceattr->config.bit_width);
                } else {
                    deviceattr->config.ch_info = sAttr->out_media_config.ch_info;
                    deviceattr->config.sample_rate = sAttr->out_media_config.sample_rate;
                    deviceattr->config.bit_width = sAttr->out_media_config.bit_width;
                    deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;

                    PAL_DBG(LOG_TAG, "PAL_DEVICE_OUT_PROXY sample rate %d bitwidth %d",
                            deviceattr->config.sample_rate, deviceattr->config.bit_width);
                }
            }
            else
            {
                /* For PAL_DEVICE_OUT_PROXY, copy all config from stream attributes */
                deviceattr->config.ch_info =sAttr->out_media_config.ch_info;
                deviceattr->config.sample_rate = sAttr->out_media_config.sample_rate;
                deviceattr->config.bit_width = sAttr->out_media_config.bit_width;
                deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;

                PAL_DBG(LOG_TAG, "PAL_DEVICE_OUT_PROXY sample rate %d bitwidth %d",
                        deviceattr->config.sample_rate, deviceattr->config.bit_width);
            }
            }
            break;
        case PAL_DEVICE_OUT_HEARING_AID:
            {
            /* For PAL_DEVICE_OUT_HEARING_AID, copy all config from stream attributes */
            deviceattr->config.ch_info = sAttr->out_media_config.ch_info;
            deviceattr->config.sample_rate = sAttr->out_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->out_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;

            PAL_DBG(LOG_TAG, "PAL_DEVICE_OUT_HEARING_AID sample rate %d bitwidth %d",
                    deviceattr->config.sample_rate, deviceattr->config.bit_width);
            }
            break;
        case PAL_DEVICE_IN_TELEPHONY_RX:
            {
            /* For PAL_DEVICE_IN_TELEPHONY_RX, copy all config from stream attributes */
            deviceattr->config.ch_info = sAttr->in_media_config.ch_info;
            deviceattr->config.sample_rate = sAttr->in_media_config.sample_rate;
            deviceattr->config.bit_width = sAttr->in_media_config.bit_width;
            deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;

            PAL_DBG(LOG_TAG, "PAL_DEVICE_IN_TELEPHONY_RX sample rate %d bitwidth %d",
                    deviceattr->config.sample_rate, deviceattr->config.bit_width);
            }
            break;
        case PAL_DEVICE_OUT_AUX_DIGITAL:
        case PAL_DEVICE_OUT_AUX_DIGITAL_1:
        case PAL_DEVICE_OUT_HDMI:
            {
                std::shared_ptr<DisplayPort> dp_device;
                dp_device = std::dynamic_pointer_cast<DisplayPort>
                                    (DisplayPort::getInstance(deviceattr, rm));
                if (!dp_device) {
                    PAL_ERR(LOG_TAG, "Failed to get DisplayPort object.");
                    return -EINVAL;
                }
                /**
                 * Comparision of stream channel and device supported max channel.
                 * If stream channel is less than or equal to device supported
                 * channel then the channel of stream is taken othewise it is of
                 * device
                 */
                int channels = dp_device->getMaxChannel();

                if (channels > sAttr->out_media_config.ch_info.channels)
                    channels = sAttr->out_media_config.ch_info.channels;

                dev_ch_info.channels = channels;

                getChannelMap(&(dev_ch_info.ch_map[0]), channels);
                deviceattr->config.ch_info = dev_ch_info;
                PAL_DBG(LOG_TAG, "Channel map set for %d", channels);

                if (dp_device->isSupportedSR(NULL,
                            sAttr->out_media_config.sample_rate)) {
                    deviceattr->config.sample_rate =
                            sAttr->out_media_config.sample_rate;
                } else {
                    int sr = dp_device->getHighestSupportedSR();
                    if (sAttr->out_media_config.sample_rate > sr)
                        deviceattr->config.sample_rate = sr;
                    else
                        deviceattr->config.sample_rate = SAMPLINGRATE_48K;
                }

                PAL_DBG(LOG_TAG, "SR %d", deviceattr->config.sample_rate);

                if (DisplayPort::isBitWidthSupported(
                            sAttr->out_media_config.bit_width)) {
                    deviceattr->config.bit_width =
                            sAttr->out_media_config.bit_width;
                } else {
                    int bps = dp_device->getHighestSupportedBps();
                    if (sAttr->out_media_config.bit_width > bps)
                        deviceattr->config.bit_width = bps;
                    else
                        deviceattr->config.bit_width = BITWIDTH_16;
                }
                PAL_DBG(LOG_TAG, "Bit Width %d", deviceattr->config.bit_width);

                deviceattr->config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            }
            break;
        default:
            PAL_ERR(LOG_TAG, "No matching device id %d", deviceattr->id);
            status = -EINVAL;
            //do nothing for rest of the devices
            break;
    }
    return status;
}

bool ResourceManager::isStreamSupported(struct pal_stream_attributes *attributes,
                                        struct pal_device *devices, int no_of_devices)
{
    bool result = false;
    uint16_t channels;
    uint32_t samplerate, bitwidth;
    uint32_t rc;
    size_t cur_sessions = 0;
    size_t max_sessions = 0;

    if (!attributes || ((no_of_devices > 0) && !devices)) {
        PAL_ERR(LOG_TAG, "Invalid input parameter attr %p, noOfDevices %d devices %p",
                attributes, no_of_devices, devices);
        return result;
    }

    // check if stream type is supported
    // and new stream session is allowed
    pal_stream_type_t type = attributes->type;
    PAL_DBG(LOG_TAG, "Enter. type %d", type);
    switch (type) {
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_VOIP:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
            cur_sessions = active_streams_ll.size();
            max_sessions = MAX_SESSIONS_LOW_LATENCY;
            break;
        case PAL_STREAM_ULTRA_LOW_LATENCY:
            cur_sessions = active_streams_ull.size();
            max_sessions = MAX_SESSIONS_ULTRA_LOW_LATENCY;
            break;
        case PAL_STREAM_DEEP_BUFFER:
            cur_sessions = active_streams_db.size();
            max_sessions = MAX_SESSIONS_DEEP_BUFFER;
            break;
        case PAL_STREAM_COMPRESSED:
            cur_sessions = active_streams_comp.size();
            max_sessions = MAX_SESSIONS_COMPRESSED;
            break;
        case PAL_STREAM_GENERIC:
            cur_sessions = active_streams_ulla.size();
            max_sessions = MAX_SESSIONS_GENERIC;
            break;
        case PAL_STREAM_RAW:
        case PAL_STREAM_VOICE_ACTIVATION:
        case PAL_STREAM_LOOPBACK:
        case PAL_STREAM_TRANSCODE:
        case PAL_STREAM_VOICE_UI:
            cur_sessions = active_streams_st.size();
            max_sessions = MAX_SESSIONS_VOICE_UI;
            break;
        case PAL_STREAM_PCM_OFFLOAD:
            cur_sessions = active_streams_po.size();
            max_sessions = MAX_SESSIONS_PCM_OFFLOAD;
            break;
        case PAL_STREAM_PROXY:
            cur_sessions = active_streams_proxy.size();
            max_sessions = MAX_SESSIONS_PROXY;
            break;
         case PAL_STREAM_VOICE_CALL:
            break;
        case PAL_STREAM_VOICE_CALL_MUSIC:
            cur_sessions = active_streams_incall_music.size();
            max_sessions = MAX_SESSIONS_INCALL_MUSIC;
            break;
        case PAL_STREAM_VOICE_CALL_RECORD:
            cur_sessions = active_streams_incall_record.size();
            max_sessions = MAX_SESSIONS_INCALL_RECORD;
            break;
        case PAL_STREAM_NON_TUNNEL:
            cur_sessions = active_streams_non_tunnel.size();
            max_sessions = max_nt_sessions;
            break;

        default:
            PAL_ERR(LOG_TAG, "Invalid stream type = %d", type);
        return result;
    }
    if (cur_sessions == max_sessions && type != PAL_STREAM_VOICE_CALL) {
        PAL_ERR(LOG_TAG, "no new session allowed for stream %d", type);
        return result;
    }

    // check if param supported by audio configruation
    switch (type) {
        case PAL_STREAM_VOICE_CALL_RECORD:
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_ULTRA_LOW_LATENCY:
        case PAL_STREAM_DEEP_BUFFER:
        case PAL_STREAM_GENERIC:
        case PAL_STREAM_VOIP:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
        case PAL_STREAM_PCM_OFFLOAD:
        case PAL_STREAM_LOOPBACK:
        case PAL_STREAM_PROXY:
        case PAL_STREAM_VOICE_CALL_MUSIC:
            if (attributes->direction == PAL_AUDIO_INPUT) {
                channels = attributes->in_media_config.ch_info.channels;
                samplerate = attributes->in_media_config.sample_rate;
                bitwidth = attributes->in_media_config.bit_width;
            } else {
                channels = attributes->out_media_config.ch_info.channels;
                samplerate = attributes->out_media_config.sample_rate;
                bitwidth = attributes->out_media_config.bit_width;
            }
            rc = (StreamPCM::isBitWidthSupported(bitwidth) |
                  StreamPCM::isSampleRateSupported(samplerate) |
                  StreamPCM::isChannelSupported(channels));
            if (0 != rc) {
               PAL_ERR(LOG_TAG, "config not supported rc %d", rc);
               return result;
            }
            PAL_INFO(LOG_TAG, "config suppported");
            result = true;
            break;
        case PAL_STREAM_COMPRESSED:
            if (attributes->direction == PAL_AUDIO_INPUT) {
               channels = attributes->in_media_config.ch_info.channels;
               samplerate = attributes->in_media_config.sample_rate;
               bitwidth = attributes->in_media_config.bit_width;
            } else {
               channels = attributes->out_media_config.ch_info.channels;
               samplerate = attributes->out_media_config.sample_rate;
               bitwidth = attributes->out_media_config.bit_width;
            }
            rc = (StreamCompress::isBitWidthSupported(bitwidth) |
                  StreamCompress::isSampleRateSupported(samplerate) |
                  StreamCompress::isChannelSupported(channels));
            if (0 != rc) {
               PAL_ERR(LOG_TAG, "config not supported rc %d", rc);
               return result;
            }
            result = true;
            break;
        case PAL_STREAM_VOICE_UI:
            if (attributes->direction == PAL_AUDIO_INPUT) {
               channels = attributes->in_media_config.ch_info.channels;
               samplerate = attributes->in_media_config.sample_rate;
               bitwidth = attributes->in_media_config.bit_width;
            } else {
               channels = attributes->out_media_config.ch_info.channels;
               samplerate = attributes->out_media_config.sample_rate;
               bitwidth = attributes->out_media_config.bit_width;
            }
            rc = (StreamSoundTrigger::isBitWidthSupported(bitwidth) |
                  StreamSoundTrigger::isSampleRateSupported(samplerate) |
                  StreamSoundTrigger::isChannelSupported(channels));
            if (0 != rc) {
               PAL_ERR(LOG_TAG, "config not supported rc %d", rc);
               return result;
            }
            PAL_INFO(LOG_TAG, "config suppported");
            result = true;
            break;
        case PAL_STREAM_VOICE_CALL:
            channels = attributes->out_media_config.ch_info.channels;
            samplerate = attributes->out_media_config.sample_rate;
            bitwidth = attributes->out_media_config.bit_width;
            rc = (StreamPCM::isBitWidthSupported(bitwidth) |
                  StreamPCM::isSampleRateSupported(samplerate) |
                  StreamPCM::isChannelSupported(channels));
            if (0 != rc) {
               PAL_ERR(LOG_TAG, "config not supported rc %d", rc);
               return result;
            }
            PAL_INFO(LOG_TAG, "config suppported");
            result = true;
            break;
        case PAL_STREAM_NON_TUNNEL:
            if (attributes->direction != PAL_AUDIO_INPUT_OUTPUT) {
               PAL_ERR(LOG_TAG, "config dir %d not supported", attributes->direction);
               return result;
            }
            PAL_INFO(LOG_TAG, "config suppported");
            result = true;
            break;
        default:
            PAL_ERR(LOG_TAG, "unknown type");
            return false;
    }
    PAL_DBG(LOG_TAG, "Exit. result %d", result);
    return result;
}

template <class T>
int registerstream(T s, std::vector<T> &streams)
{
    int ret = 0;
    streams.push_back(s);
    return ret;
}

int ResourceManager::registerStream(Stream *s)
{
    int ret = 0;
    pal_stream_type_t type;
    PAL_DBG(LOG_TAG, "Enter. stream %pK", s);
    ret = s->getStreamType(&type);
    if (0 != ret) {
        PAL_ERR(LOG_TAG, "getStreamType failed with status = %d", ret);
        return ret;
    }
    PAL_DBG(LOG_TAG, "stream type %d", type);
    mResourceManagerMutex.lock();
    switch (type) {
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
        case PAL_STREAM_VOICE_CALL:
        {
            StreamPCM* sPCM = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sPCM, active_streams_ll);
            break;
        }
        case PAL_STREAM_PCM_OFFLOAD:
        case PAL_STREAM_LOOPBACK:
        {
            StreamPCM* sPCM = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sPCM, active_streams_po);
            break;
        }
        case PAL_STREAM_DEEP_BUFFER:
        {
            StreamPCM* sDB = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sDB, active_streams_db);
            break;
        }
        case PAL_STREAM_COMPRESSED:
        {
            StreamCompress* sComp = dynamic_cast<StreamCompress*>(s);
            ret = registerstream(sComp, active_streams_comp);
            break;
        }
        case PAL_STREAM_GENERIC:
        {
            StreamPCM* sULLA = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sULLA, active_streams_ulla);
            break;
        }
        case PAL_STREAM_VOICE_UI:
        {
            StreamSoundTrigger* sST = dynamic_cast<StreamSoundTrigger*>(s);
            ret = registerstream(sST, active_streams_st);
            break;
        }
        case PAL_STREAM_ULTRA_LOW_LATENCY:
        {
            StreamPCM* sULL = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sULL, active_streams_ull);
            break;
        }
        case PAL_STREAM_PROXY:
        {
            StreamPCM* sProxy = dynamic_cast<StreamPCM*>(s);
            ret = registerstream(sProxy, active_streams_proxy);
            break;
        }
        case PAL_STREAM_VOICE_CALL_MUSIC:
        {
            StreamInCall* sPCM = dynamic_cast<StreamInCall*>(s);
            ret = registerstream(sPCM, active_streams_incall_music);
            break;
        }
        case PAL_STREAM_VOICE_CALL_RECORD:
        {
            StreamInCall* sPCM = dynamic_cast<StreamInCall*>(s);
            ret = registerstream(sPCM, active_streams_incall_record);
            break;
        }
        case PAL_STREAM_NON_TUNNEL:
        {
            StreamNonTunnel* sNonTunnel = dynamic_cast<StreamNonTunnel*>(s);
            ret = registerstream(sNonTunnel, active_streams_non_tunnel);
            break;
        }

        default:
            ret = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid stream type = %d ret %d", type, ret);
            break;
    }
    mActiveStreams.push_back(s);

#if 0
    s->getStreamAttributes(&incomingStreamAttr);
    int incomingPriority = getStreamAttrPriority(incomingStreamAttr);
    if (incomingPriority > mPriorityHighestPriorityActiveStream) {
        PAL_INFO(LOG_TAG, "Added new stream with highest priority %d", incomingPriority);
        mPriorityHighestPriorityActiveStream = incomingPriority;
        mHighestPriorityActiveStream = s;
    }
    calculalte priority and store in Stream

    mAllActiveStreams.push_back(s);
#endif

    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

///private functions


// template function to deregister stream
template <class T>
int deregisterstream(T s, std::vector<T> &streams)
{
    int ret = 0;
    typename std::vector<T>::iterator iter = std::find(streams.begin(), streams.end(), s);
    if (iter != streams.end())
        streams.erase(iter);
    else
        ret = -ENOENT;
    return ret;
}

int ResourceManager::deregisterStream(Stream *s)
{
    int ret = 0;
    pal_stream_type_t type;
    PAL_DBG(LOG_TAG, "Enter. stream %pK", s);
    ret = s->getStreamType(&type);
    if (0 != ret) {
        PAL_ERR(LOG_TAG, "getStreamType failed with status = %d", ret);
        return ret;
    }
#if 0
    remove s from mAllActiveStreams
    get priority from remaining streams and find highest priority stream
    and store in mHighestPriorityActiveStream
#endif
    PAL_INFO(LOG_TAG, "stream type %d", type);
    mResourceManagerMutex.lock();
    switch (type) {
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
        case PAL_STREAM_VOICE_CALL:
        {
            StreamPCM* sPCM = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sPCM, active_streams_ll);
            break;
        }
        case PAL_STREAM_PCM_OFFLOAD:
        case PAL_STREAM_LOOPBACK:
        {
            StreamPCM* sPCM = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sPCM, active_streams_po);
            break;
        }
        case PAL_STREAM_DEEP_BUFFER:
        {
            StreamPCM* sDB = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sDB, active_streams_db);
            break;
        }
        case PAL_STREAM_COMPRESSED:
        {
            StreamCompress* sComp = dynamic_cast<StreamCompress*>(s);
            ret = deregisterstream(sComp, active_streams_comp);
            break;
        }
        case PAL_STREAM_GENERIC:
        {
            StreamPCM* sULLA = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sULLA, active_streams_ulla);
            break;
        }
        case PAL_STREAM_VOICE_UI:
        {
            StreamSoundTrigger* sST = dynamic_cast<StreamSoundTrigger*>(s);
            ret = deregisterstream(sST, active_streams_st);
            // reset concurrency count when all st streams deregistered
            if (active_streams_st.size() == 0) {
                concurrencyDisableCount = 0;
                concurrencyEnableCount = 0;
            }
            break;
        }
        case PAL_STREAM_ULTRA_LOW_LATENCY:
        {
            StreamPCM* sULL = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sULL, active_streams_ull);
            break;
        }
        case PAL_STREAM_PROXY:
        {
            StreamPCM* sProxy = dynamic_cast<StreamPCM*>(s);
            ret = deregisterstream(sProxy, active_streams_proxy);
            break;
        }
        case PAL_STREAM_VOICE_CALL_MUSIC:
        {
            StreamInCall* sPCM = dynamic_cast<StreamInCall*>(s);
            ret = deregisterstream(sPCM, active_streams_incall_music);
            break;
        }
        case PAL_STREAM_VOICE_CALL_RECORD:
        {
            StreamInCall* sPCM = dynamic_cast<StreamInCall*>(s);
            ret = deregisterstream(sPCM, active_streams_incall_record);
            break;
        }
        case PAL_STREAM_NON_TUNNEL:
        {
            StreamNonTunnel* sNonTunnel = dynamic_cast<StreamNonTunnel*>(s);
            ret = deregisterstream(sNonTunnel, active_streams_non_tunnel);
            break;
        }
        default:
            ret = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid stream type = %d ret %d", type, ret);
            break;
    }

    deregisterstream(s, mActiveStreams);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

template <class T>
bool isStreamActive(T s, std::vector<T> &streams)
{
    bool ret = false;

    PAL_DBG(LOG_TAG, "Enter.");
    typename std::vector<T>::iterator iter =
        std::find(streams.begin(), streams.end(), s);
    if (iter != streams.end()) {
        ret = true;
    }

    return ret;
}

int ResourceManager::registerDevice_l(std::shared_ptr<Device> d, Stream *s)
{
    PAL_DBG(LOG_TAG, "Enter.");
    active_devices.push_back(std::make_pair(d, s));
    PAL_DBG(LOG_TAG, "Exit.");
    return 0;
}

int ResourceManager::registerDevice(std::shared_ptr<Device> d, Stream *s)
{
    int status = 0;
    struct pal_stream_attributes sAttr;
    std::shared_ptr<Device> dev = nullptr;
    std::vector<std::shared_ptr<Device>> associatedDevices;
    std::vector<Stream*> str_list;
    std::vector <Stream *> activeStreams;
    int rxdevcount = 0;
    struct pal_stream_attributes rx_attr;

    PAL_DBG(LOG_TAG, "Enter. dev id: %d", d->getSndDeviceId());
    status = s->getStreamAttributes(&sAttr);
    if (status != 0) {
        PAL_ERR(LOG_TAG,"stream get attributes failed");
        return status;
    }

    mResourceManagerMutex.lock();
    registerDevice_l(d, s);
    if (sAttr.direction == PAL_AUDIO_INPUT &&
        sAttr.type != PAL_STREAM_PROXY &&
        sAttr.type != PAL_STREAM_ULTRA_LOW_LATENCY &&
        sAttr.type != PAL_STREAM_GENERIC ) {
        dev = getActiveEchoReferenceRxDevices_l(s);
        if (dev) {
            // use setECRef_l to avoid deadlock
            mResourceManagerMutex.unlock();
            getActiveStream_l(dev, activeStreams);
            for (auto& rx_str: activeStreams) {
                 rx_str->getStreamAttributes(&rx_attr);
                 if (rx_attr.direction != PAL_AUDIO_INPUT) {
                     if (getEcRefStatus(sAttr.type, rx_attr.type)) {
                         rxdevcount++;
                     } else {
                         PAL_DBG(LOG_TAG, "rx stream is disabled for ec ref %d", rx_attr.type);
                         continue;
                     }
                 } else {
                     PAL_DBG(LOG_TAG, "Not rx stream type %d", rx_attr.type);
                     continue;
                 }
            }
            status = s->setECRef_l(dev, true);
            mResourceManagerMutex.lock();
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to enable EC Ref");
            } else {
               for (int i = 0; i < rxdevcount; i++) {
                    dev->setEcRefDevCount(true, false);
               }
            }
        }
    } else if (sAttr.direction == PAL_AUDIO_OUTPUT &&
        sAttr.type != PAL_STREAM_PROXY &&
        sAttr.type != PAL_STREAM_ULTRA_LOW_LATENCY) {
        status = s->getAssociatedDevices(associatedDevices);
        if (0 != status) {
            PAL_ERR(LOG_TAG,"getAssociatedDevices Failed\n");
            return status;
        }

        for (auto dev: associatedDevices) {
            str_list = getConcurrentTxStream_l(s, dev);
            for (auto str: str_list) {
                PAL_DBG(LOG_TAG, "Enter enable EC Ref");
                if (str && isStreamActive(str, mActiveStreams)) {
                    mResourceManagerMutex.unlock();
                    status = str->setECRef(dev, true);
                    mResourceManagerMutex.lock();
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to enable EC Ref");
                    } else if (dev) {
                       dev->setEcRefDevCount(true, false);
                    }
                }
            }
        }
    }
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit. ret: %d", status);
    return status;
}

int ResourceManager::deregisterDevice_l(std::shared_ptr<Device> d, Stream *s)
{
    int ret = 0;
    PAL_VERBOSE(LOG_TAG, "Enter.");

    auto iter = std::find(active_devices.begin(),
        active_devices.end(), std::make_pair(d, s));
    if (iter != active_devices.end())
        active_devices.erase(iter);
    else {
        ret = -ENOENT;
        PAL_ERR(LOG_TAG, "no device %d found in active device list ret %d",
                d->getSndDeviceId(), ret);
    }
    PAL_VERBOSE(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

int ResourceManager::deregisterDevice(std::shared_ptr<Device> d, Stream *s)
{
    int status = 0;
    struct pal_stream_attributes sAttr;
    std::shared_ptr<Device> dev = nullptr;
    std::vector<std::shared_ptr<Device>> associatedDevices;
    std::vector<Stream*> str_list;

    PAL_DBG(LOG_TAG, "Enter. dev id: %d", d->getSndDeviceId());
    status = s->getStreamAttributes(&sAttr);
    if (status != 0) {
        PAL_ERR(LOG_TAG,"stream get attributes failed");
        return status;
    }

    mResourceManagerMutex.lock();
    if (sAttr.direction == PAL_AUDIO_INPUT) {
        dev = getActiveEchoReferenceRxDevices_l(s);
        mResourceManagerMutex.unlock();
        status = s->setECRef_l(dev, false);
        mResourceManagerMutex.lock();
        if (status) {
            PAL_ERR(LOG_TAG, "Failed to disable EC Ref");
        } else if (dev) {
           dev->setEcRefDevCount(false, true);
        }
    } else if (sAttr.direction == PAL_AUDIO_OUTPUT) {
        status = s->getAssociatedDevices(associatedDevices);
        if (0 != status) {
            PAL_ERR(LOG_TAG,"getAssociatedDevices Failed\n");
            return status;
        }

        for (auto dev: associatedDevices) {
            str_list = getConcurrentTxStream_l(s, dev);
            for (auto str: str_list) {
                /*
                 * NOTE: this check works based on sequence that
                 * Rx stream stops device after stopping session
                 */
                if (dev->getEcRefDevCount() > 1) {
                    PAL_DBG(LOG_TAG, "Rx dev still active, ignore set ECRef");
                } else if (str && isStreamActive(str, mActiveStreams)) {
                    mResourceManagerMutex.unlock();
                    status = str->setECRef(dev, false);
                    mResourceManagerMutex.lock();
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to disable EC Ref");
                    } else if (dev) {
                          dev->setEcRefDevCount(false, false);
                    }
                }
            }
        }
    }
    deregisterDevice_l(d, s);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit. ret: %d", status);
    return status;
}

bool ResourceManager::isDeviceActive(std::shared_ptr<Device> d, Stream *s)
{
    bool is_active = false;

    PAL_DBG(LOG_TAG, "Enter.");
    mResourceManagerMutex.lock();
    is_active = isDeviceActive_l(d, s);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit.");
    return is_active;
}

bool ResourceManager::isDeviceActive_l(std::shared_ptr<Device> d, Stream *s)
{
    bool is_active = false;
    int deviceId = d->getSndDeviceId();

    PAL_DBG(LOG_TAG, "Enter.");
    auto iter = std::find(active_devices.begin(),
        active_devices.end(), std::make_pair(d, s));
    if (iter != active_devices.end()) {
        is_active = true;
    }

    PAL_DBG(LOG_TAG, "Exit. device %d is active %d", deviceId, is_active);
    return is_active;
}

int ResourceManager::addPlugInDevice(std::shared_ptr<Device> d,
                            pal_param_device_connection_t connection_state)
{
    int ret = 0;

    ret = d->init(connection_state);
    if (ret) {
        PAL_ERR(LOG_TAG, "failed to init deivce.");
        return ret;
    }

    plugin_devices_.push_back(d);
    return 0;
}

int ResourceManager::removePlugInDevice(pal_device_id_t device_id,
                            pal_param_device_connection_t connection_state)
{
    int ret = 0;
    PAL_DBG(LOG_TAG, "Enter.");
    typename std::vector<std::shared_ptr<Device>>::iterator iter;

    for (iter = plugin_devices_.begin(); iter != plugin_devices_.end(); iter++) {
        if ((*iter)->getSndDeviceId() == device_id)
            break;
    }

    if (iter != plugin_devices_.end()) {
        (*iter)->deinit(connection_state);
        plugin_devices_.erase(iter);
    } else {
        ret = -ENOENT;
        PAL_ERR(LOG_TAG, "no device %d found in plugin device list ret %d",
                device_id, ret);
    }
    PAL_DBG(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

int ResourceManager::getActiveDevices(std::vector<std::shared_ptr<Device>> &deviceList)
{
    int ret = 0;
    mResourceManagerMutex.lock();
    for (int i = 0; i < active_devices.size(); i++)
        deviceList.push_back(active_devices[i].first);
    mResourceManagerMutex.unlock();
    return ret;
}

int ResourceManager::getAudioRoute(struct audio_route** ar)
{
    if (!audio_route) {
        PAL_ERR(LOG_TAG, "no audio route found");
        return -ENOENT;
    }
    *ar = audio_route;
    PAL_DBG(LOG_TAG, "ar %pK audio_route %pK", ar, audio_route);
    return 0;
}

int ResourceManager::getAudioMixer(struct audio_mixer ** am)
{
    if (!audio_mixer || !am) {
        PAL_ERR(LOG_TAG, "no audio mixer found");
        return -ENOENT;
    }
    *am = audio_mixer;
    PAL_DBG(LOG_TAG, "ar %pK audio_mixer %pK", am, audio_mixer);
    return 0;
}

void ResourceManager::GetVoiceUIProperties(struct pal_st_properties *qstp)
{
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (!qstp) {
        return;
    }

    memcpy(qstp, &qst_properties, sizeof(struct pal_st_properties));

    if (st_info) {
        qstp->concurrent_capture = st_info->GetConcurrentCaptureEnable();
    }
}

bool ResourceManager::IsVoiceUILPISupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetLpiEnable();
    } else {
        return false;
    }
}

// this should only be called when LPI supported by platform
void ResourceManager::GetSVAConcurrencyCount(
    int32_t *enable_count, int32_t *disable_count) {

    pal_stream_attributes st_attr;
    bool voice_conc_enable = IsVoiceCallAndVoiceUIConcurrencySupported();
    bool voip_conc_enable = IsVoipAndVoiceUIConcurrencySupported();
    bool audio_capture_conc_enable =
        IsAudioCaptureAndVoiceUIConcurrencySupported();
    bool low_latency_bargein_enable = IsLowLatencyBargeinSupported();

    mResourceManagerMutex.lock();
    if (concurrencyEnableCount > 0 || concurrencyDisableCount > 0) {
        PAL_DBG(LOG_TAG, "Concurrency count already updated, return");
        goto exit;
    }

    for (auto& s: mActiveStreams) {
        if (!s->isActive()) {
            continue;
        }
        s->getStreamAttributes(&st_attr);

        if (st_attr.type == PAL_STREAM_VOICE_CALL) {
            if (!voice_conc_enable) {
                concurrencyDisableCount++;
            } else {
                concurrencyEnableCount++;
            }
        } else if (st_attr.type == PAL_STREAM_VOIP_TX ||
                st_attr.type == PAL_STREAM_VOIP_RX ||
                st_attr.type == PAL_STREAM_VOIP) {
            if (!voip_conc_enable) {
                concurrencyDisableCount++;
            } else {
                concurrencyEnableCount++;
            }
        } else if (st_attr.direction == PAL_AUDIO_INPUT &&
                   (st_attr.type == PAL_STREAM_LOW_LATENCY ||
                    st_attr.type == PAL_STREAM_DEEP_BUFFER)) {
            if (!audio_capture_conc_enable) {
                concurrencyDisableCount++;
            } else {
                concurrencyEnableCount++;
            }
        } else if (st_attr.direction == PAL_AUDIO_OUTPUT &&
                   (st_attr.type != PAL_STREAM_LOW_LATENCY ||
                    low_latency_bargein_enable)) {
            concurrencyEnableCount++;
        }
    }

exit:
    mResourceManagerMutex.unlock();
    *enable_count = concurrencyEnableCount;
    *disable_count = concurrencyDisableCount;
    PAL_INFO(LOG_TAG, "conc enable cnt %d, conc disable count %d",
        *enable_count, *disable_count);

}

bool ResourceManager::IsLowLatencyBargeinSupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetLowLatencyBargeinEnable();
    }
    return false;
}

bool ResourceManager::IsAudioCaptureAndVoiceUIConcurrencySupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetConcurrentCaptureEnable();
    }
    return false;
}

bool ResourceManager::IsVoiceCallAndVoiceUIConcurrencySupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetConcurrentVoiceCallEnable();
    }
    return false;
}

bool ResourceManager::IsVoipAndVoiceUIConcurrencySupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetConcurrentVoipCallEnable();
    }
    return false;
}

bool ResourceManager::IsTransitToNonLPIOnChargingSupported() {
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (st_info) {
        return st_info->GetTransitToNonLpiOnCharging();
    }
    return false;
}

bool ResourceManager::CheckForForcedTransitToNonLPI() {
    if (IsTransitToNonLPIOnChargingSupported() && charging_state_)
      return true;

    return false;
}

std::shared_ptr<CaptureProfile> ResourceManager::GetCaptureProfileByPriority(
    StreamSoundTrigger *s) {
    std::shared_ptr<CaptureProfile> cap_prof = nullptr;
    std::shared_ptr<CaptureProfile> cap_prof_priority = nullptr;

    for (int i = 0; i < active_streams_st.size(); i++) {
        // NOTE: input param s can be nullptr here
        if (active_streams_st[i] == s) {
            continue;
        }

        /*
         * Ignore capture profile for streams in below states:
         * 1. sound model loaded but not started by sthal
         * 2. stop recognition called by sthal
         */
        if (!active_streams_st[i]->isActive())
            continue;

        cap_prof = active_streams_st[i]->GetCurrentCaptureProfile();
        if (!cap_prof) {
            PAL_ERR(LOG_TAG, "Failed to get capture profile");
            continue;
        } else if (cap_prof->ComparePriority(cap_prof_priority) ==
                   CAPTURE_PROFILE_PRIORITY_HIGH) {
            cap_prof_priority = cap_prof;
        }
    }

    return cap_prof_priority;
}

bool ResourceManager::UpdateSVACaptureProfile(StreamSoundTrigger *s, bool is_active) {

    bool backend_update = false;
    std::shared_ptr<CaptureProfile> cap_prof = nullptr;
    std::shared_ptr<CaptureProfile> cap_prof_priority = nullptr;

    if (!s) {
        PAL_ERR(LOG_TAG, "Invalid stream");
        return false;
    }

    // backend config update
    if (is_active) {
        cap_prof = s->GetCurrentCaptureProfile();
        if (!cap_prof) {
            PAL_ERR(LOG_TAG, "Failed to get capture profile");
            return false;
        }

        if (!SVACaptureProfile) {
            SVACaptureProfile = cap_prof;
        } else if (SVACaptureProfile->ComparePriority(cap_prof) < 0){
            SVACaptureProfile = cap_prof;
            backend_update = true;
        }
    } else {
        cap_prof_priority = GetCaptureProfileByPriority(s);

        if (!cap_prof_priority) {
            PAL_DBG(LOG_TAG, "No SVA session active, reset capture profile");
            SVACaptureProfile = nullptr;
        } else if (cap_prof_priority->ComparePriority(SVACaptureProfile) != 0) {
            SVACaptureProfile = cap_prof_priority;
            backend_update = true;
        }

    }

    return backend_update;
}

int ResourceManager::SwitchSVADevices(bool connect_state,
    pal_device_id_t device_id) {
    int32_t status = 0;
    pal_device_id_t dest_device;
    pal_device_id_t device_to_disconnect;
    pal_device_id_t device_to_connect;
    std::shared_ptr<CaptureProfile> cap_prof_priority = nullptr;
    StreamSoundTrigger *st_str = nullptr;
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    PAL_DBG(LOG_TAG, "Enter");

    if (!st_info->GetSupportDevSwitch()) {
        PAL_INFO(LOG_TAG, "Device switch not supported, return");
        return 0;
    }

    // TODO: add support for other devices
    if (device_id == PAL_DEVICE_IN_HANDSET_MIC ||
        device_id == PAL_DEVICE_IN_SPEAKER_MIC) {
        dest_device = PAL_DEVICE_IN_HANDSET_VA_MIC;
    } else if (device_id == PAL_DEVICE_IN_WIRED_HEADSET) {
        dest_device = PAL_DEVICE_IN_HEADSET_VA_MIC;
    } else {
        PAL_DBG(LOG_TAG, "Unsupported device %d", device_id);
        return status;
    }

    cap_prof_priority = GetCaptureProfileByPriority(nullptr);

    if (!cap_prof_priority) {
        PAL_DBG(LOG_TAG, "No SVA session active, reset capture profile");
        SVACaptureProfile = nullptr;
    } else if (cap_prof_priority->ComparePriority(SVACaptureProfile) ==
               CAPTURE_PROFILE_PRIORITY_HIGH) {
        SVACaptureProfile = cap_prof_priority;
    }

    // TODO: add support for other devices
    if (connect_state && dest_device == PAL_DEVICE_IN_HEADSET_VA_MIC) {
        device_to_connect = dest_device;
        device_to_disconnect = PAL_DEVICE_IN_HANDSET_VA_MIC;
    } else if (!connect_state && dest_device == PAL_DEVICE_IN_HEADSET_VA_MIC) {
        device_to_connect = PAL_DEVICE_IN_HANDSET_VA_MIC;
        device_to_disconnect = dest_device;
    }

    for (int i = 0; i < active_streams_st.size(); i++) {
        st_str = active_streams_st[i];
        if (st_str && isStreamActive(st_str, active_streams_st)) {
            mResourceManagerMutex.unlock();
            status = active_streams_st[i]->DisconnectDevice(device_to_disconnect);
            mResourceManagerMutex.lock();

            if (status) {
                PAL_ERR(LOG_TAG, "Failed to disconnect device %d for SVA",
                    device_to_disconnect);
            }
        }
    }
    for (int i = 0; i < active_streams_st.size(); i++) {
        st_str = active_streams_st[i];
        if (st_str && isStreamActive(st_str, active_streams_st)) {
            mResourceManagerMutex.unlock();
            status = active_streams_st[i]->ConnectDevice(device_to_connect);
            mResourceManagerMutex.lock();
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to connect device %d for SVA",
                    device_to_connect);
            }
        }
    }
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

std::shared_ptr<CaptureProfile> ResourceManager::GetSVACaptureProfile() {
    return SVACaptureProfile;
}

/* NOTE: there should be only one callback for each pcm id
 * so when new different callback register with same pcm id
 * older one will be overwritten
 */
int ResourceManager::registerMixerEventCallback(const std::vector<int> &DevIds,
                                                session_callback callback,
                                                uint64_t cookie,
                                                bool is_register) {
    int status = 0;
    std::map<int, std::pair<session_callback, uint64_t>>::iterator it;

    if (!callback || DevIds.size() <= 0) {
        PAL_ERR(LOG_TAG, "Invalid callback or pcm ids");
        return -EINVAL;
    }

    if (mixerEventRegisterCount == 0 && !is_register) {
        PAL_ERR(LOG_TAG, "Cannot deregister unregistered callback");
        return -EINVAL;
    }

    if (is_register) {
        for (int i = 0; i < DevIds.size(); i++) {
            it = mixerEventCallbackMap.find(DevIds[i]);
            if (it != mixerEventCallbackMap.end()) {
                PAL_DBG(LOG_TAG, "callback exists for pcm id %d, overwrite",
                    DevIds[i]);
                mixerEventCallbackMap.erase(it);
            }
            mixerEventCallbackMap.insert(std::make_pair(DevIds[i],
                std::make_pair(callback, cookie)));

        }
        mixerEventRegisterCount++;
    } else {
        for (int i = 0; i < DevIds.size(); i++) {
            it = mixerEventCallbackMap.find(DevIds[i]);
            if (it != mixerEventCallbackMap.end()) {
                PAL_DBG(LOG_TAG, "callback found for pcm id %d, remove",
                    DevIds[i]);
                if (callback == it->second.first) {
                    mixerEventCallbackMap.erase(it);
                } else {
                    PAL_ERR(LOG_TAG, "No matching callback found for pcm id %d",
                        DevIds[i]);
                }
            } else {
                PAL_ERR(LOG_TAG, "No callback found for pcm id %d", DevIds[i]);
            }
        }
        mixerEventRegisterCount--;
    }

    return status;
}

void ResourceManager::mixerEventWaitThreadLoop(
    std::shared_ptr<ResourceManager> rm) {
    int ret = 0;
    struct snd_ctl_event mixer_event = {0, {.data8 = {0}}};
    struct mixer *mixer = nullptr;

    ret = rm->getAudioMixer(&mixer);
    if (ret) {
        PAL_ERR(LOG_TAG, "Failed to get audio mxier");
        return;
    }

    PAL_INFO(LOG_TAG, "subscribing for event");
    mixer_subscribe_events(mixer, 1);

    while (1) {
        PAL_VERBOSE(LOG_TAG, "going to wait for event");
        ret = mixer_wait_event(mixer, -1);
        PAL_VERBOSE(LOG_TAG, "mixer_wait_event returns %d", ret);
        if (ret <= 0) {
            PAL_DBG(LOG_TAG, "mixer_wait_event err! ret = %d", ret);
        } else if (ret > 0) {
            ret = mixer_read_event(mixer, &mixer_event);
            if (ret >= 0) {
                if (strstr((char *)mixer_event.data.elem.id.name, (char *)"event")) {
                    PAL_INFO(LOG_TAG, "Event Received %s",
                             mixer_event.data.elem.id.name);
                    ret = rm->handleMixerEvent(mixer,
                        (char *)mixer_event.data.elem.id.name);
                } else
                    PAL_VERBOSE(LOG_TAG, "Unwanted event, Skipping");
            } else {
                PAL_DBG(LOG_TAG, "mixer_read failed, ret = %d", ret);
            }
        }
        if (ResourceManager::mixerClosed) {
            PAL_INFO(LOG_TAG, "mixerClosed, exit mixerEventWaitThreadLoop");
            return;
        }
    }
    PAL_INFO(LOG_TAG, "unsubscribing for event");
    mixer_subscribe_events(mixer, 0);
}

int ResourceManager::handleMixerEvent(struct mixer *mixer, char *mixer_str) {
    int status = 0;
    int pcm_id = 0;
    uint64_t cookie = 0;
    session_callback session_cb = nullptr;
    std::string event_str(mixer_str);
    // TODO: hard code in common defs
    std::string pcm_prefix = "PCM";
    std::string compress_prefix = "COMPRESS";
    std::string event_suffix = "event";
    size_t prefix_idx = 0;
    size_t suffix_idx = 0;
    size_t length = 0;
    struct mixer_ctl *ctl = nullptr;
    char *buf = nullptr;
    unsigned int num_values;
    struct agm_event_cb_params *params = nullptr;
    std::map<int, std::pair<session_callback, uint64_t>>::iterator it;

    PAL_DBG(LOG_TAG, "Enter");
    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        PAL_ERR(LOG_TAG, "Invalid mixer control: %s", mixer_str);
        status = -EINVAL;
        goto exit;
    }

    // parse event payload
    num_values = mixer_ctl_get_num_values(ctl);
    PAL_VERBOSE(LOG_TAG, "num_values: %d", num_values);
    buf = (char *)calloc(1, num_values);
    if (!buf) {
        PAL_ERR(LOG_TAG, "Failed to allocate buf");
        status = -ENOMEM;
        goto exit;
    }

    status = mixer_ctl_get_array(ctl, buf, num_values);
    if (status < 0) {
        PAL_ERR(LOG_TAG, "Failed to mixer_ctl_get_array");
        goto exit;
    }

    params = (struct agm_event_cb_params *)buf;
    PAL_DBG(LOG_TAG, "source module id %x, event id %d, payload size %d",
            params->source_module_id, params->event_id,
            params->event_payload_size);

    if (!params->source_module_id || !params->event_payload_size) {
        PAL_ERR(LOG_TAG, "Invalid source module id or payload size");
        goto exit;
    }

    // NOTE: event we get should be in format like "PCM100 event"
    prefix_idx = event_str.find(pcm_prefix);
    if (prefix_idx == event_str.npos) {
        prefix_idx = event_str.find(compress_prefix);
        if (prefix_idx == event_str.npos) {
            PAL_ERR(LOG_TAG, "Invalid mixer event");
            status = -EINVAL;
            goto exit;
        } else {
            prefix_idx += compress_prefix.length();
        }
    } else {
        prefix_idx += pcm_prefix.length();
    }

    suffix_idx = event_str.find(event_suffix);
    if (suffix_idx == event_str.npos || suffix_idx - prefix_idx <= 1) {
        PAL_ERR(LOG_TAG, "Invalid mixer event");
        status = -EINVAL;
        goto exit;
    }

    length = suffix_idx - prefix_idx;
    pcm_id = std::stoi(event_str.substr(prefix_idx, length));

    // acquire callback/cookie with pcm dev id
    it = mixerEventCallbackMap.find(pcm_id);
    if (it != mixerEventCallbackMap.end()) {
        session_cb = it->second.first;
        cookie = it->second.second;
    }

    if (!session_cb) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid session callback");
        goto exit;
    }

    // callback
    session_cb(cookie, params->event_id, (void *)params->event_payload,
                 params->event_payload_size);

exit:
    if (buf)
        free(buf);
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int ResourceManager::StopOtherSVAStreams(StreamSoundTrigger *st) {
    int status = 0;
    StreamSoundTrigger *st_str = nullptr;

    mResourceManagerMutex.lock();
    for (int i = 0; i < active_streams_st.size(); i++) {
        st_str = active_streams_st[i];
        if (st_str && st_str != st &&
            isStreamActive(st_str, active_streams_st)) {
            mResourceManagerMutex.unlock();
            status = st_str->Pause();
            mResourceManagerMutex.lock();
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to pause SVA stream");
            }
        }
    }
    mResourceManagerMutex.unlock();

    return status;
}

int ResourceManager::StartOtherSVAStreams(StreamSoundTrigger *st) {
    int status = 0;
    StreamSoundTrigger *st_str = nullptr;

    mResourceManagerMutex.lock();
    for (int i = 0; i < active_streams_st.size(); i++) {
        st_str = active_streams_st[i];
        if (st_str && st_str != st &&
            isStreamActive(st_str, active_streams_st)) {
            mResourceManagerMutex.unlock();
            status = st_str->Resume();
            mResourceManagerMutex.lock();
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to do resume SVA stream");
            }
        }
    }
    mResourceManagerMutex.unlock();

    return status;
}

// apply for Voice UI streams only
void ResourceManager::ConcurrentStreamStatus(pal_stream_type_t type,
                                             pal_stream_direction_t dir,
                                             bool active) {
    int32_t status = 0;
    bool conc_en = true;
    bool do_switch = false;
    bool tx_conc = false;
    bool rx_conc = false;
    bool voice_conc_enable = IsVoiceCallAndVoiceUIConcurrencySupported();
    bool voip_conc_enable = IsVoipAndVoiceUIConcurrencySupported();
    bool audio_capture_conc_enable =
        IsAudioCaptureAndVoiceUIConcurrencySupported();
    bool low_latency_bargein_enable = IsLowLatencyBargeinSupported();
    StreamSoundTrigger *st_str = nullptr;
    std::shared_ptr<CaptureProfile> cap_prof_priority = nullptr;

    mResourceManagerMutex.lock();
    PAL_DBG(LOG_TAG, "Enter, type %d direction %d active %d", type, dir, active);

    if (dir == PAL_AUDIO_OUTPUT) {
        if (type == PAL_STREAM_LOW_LATENCY && !low_latency_bargein_enable) {
            PAL_VERBOSE(LOG_TAG, "Ignore low latency playback stream");
            goto exit;
        } else {
            rx_conc = true;
        }
    }

    if (active_streams_st.size() == 0) {
        PAL_DBG(LOG_TAG, "No need to concurrent as no SVA streams available");
        goto exit;
    }

    /*
     * Generally voip/voice rx stream comes with related tx streams,
     * so there's no need to switch to NLPI for voip/voice rx stream
     * if corresponding voip/voice tx stream concurrency is not supported.
     */
    if (type == PAL_STREAM_VOICE_CALL) {
        tx_conc = true;
        rx_conc = true;
        if (!voice_conc_enable) {
            PAL_DBG(LOG_TAG, "pause on voice concurrency");
            conc_en = false;
        }
    } else if (type == PAL_STREAM_VOIP_TX ||
               type == PAL_STREAM_VOIP_RX ||
               type == PAL_STREAM_VOIP) {
        tx_conc = true;
        rx_conc = true;
        if (!voip_conc_enable) {
            PAL_DBG(LOG_TAG, "pause on voip concurrency");
            conc_en = false;
        }
    } else if (dir == PAL_AUDIO_INPUT &&
               (type == PAL_STREAM_LOW_LATENCY ||
                type == PAL_STREAM_DEEP_BUFFER)) {
        tx_conc = true;
        if (!audio_capture_conc_enable) {
            PAL_DBG(LOG_TAG, "pause on audio capture concurrency");
            conc_en = false;
        }
    }

    PAL_INFO(LOG_TAG, "stream type %d active %d Tx conc %d, Rx conc %d, concurrency%s allowed",
        type, active, tx_conc, rx_conc, conc_en? "" : " not");

    if (!conc_en) {
        if (active) {
            ++concurrencyDisableCount;
            if (concurrencyDisableCount == 1) {
                // pause all sva streams
                for (int i = 0; i < active_streams_st.size(); i++) {
                    st_str = active_streams_st[i];
                    if (st_str &&
                        isStreamActive(st_str, active_streams_st)) {
                        mResourceManagerMutex.unlock();
                        status = st_str->Pause();
                        mResourceManagerMutex.lock();
                        if (status) {
                            PAL_ERR(LOG_TAG, "Failed to pause SVA stream");
                        }
                    }
                }
            }
        } else {
            --concurrencyDisableCount;
            if (concurrencyDisableCount == 0) {
                // resume all sva streams
                for (int i = 0; i < active_streams_st.size(); i++) {
                    st_str = active_streams_st[i];
                    if (st_str &&
                        isStreamActive(st_str, active_streams_st)) {
                        mResourceManagerMutex.unlock();
                        status = st_str->Resume();
                        mResourceManagerMutex.lock();
                        if (status) {
                            PAL_ERR(LOG_TAG, "Failed to resume SVA stream");
                        }
                    }
                }
            }
        }
    } else if (tx_conc || rx_conc) {
        if (!IsVoiceUILPISupported()) {
            PAL_INFO(LOG_TAG, "LPI not enabled by platform, skip switch");
        } else if (active) {
            if (++concurrencyEnableCount == 1) {
                do_switch = true;
            }
        } else {
            if (--concurrencyEnableCount == 0) {
                do_switch = true;
            }
        }

        if (do_switch) {
            // update use_lpi_ for all sva streams
            for (int i = 0; i < active_streams_st.size(); i++) {
                st_str = active_streams_st[i];
                if (st_str && isStreamActive(st_str, active_streams_st)) {
                    mResourceManagerMutex.unlock();
                    status = st_str->EnableLPI(!active);
                    mResourceManagerMutex.lock();
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to update LPI state");
                    }
                }
            }

            // update common SVA capture profile
            SVACaptureProfile = nullptr;
            cap_prof_priority = GetCaptureProfileByPriority(nullptr);

            if (!cap_prof_priority) {
                PAL_DBG(LOG_TAG, "No SVA session active, reset capture profile");
                SVACaptureProfile = nullptr;
            } else if (cap_prof_priority->ComparePriority(SVACaptureProfile) ==
                    CAPTURE_PROFILE_PRIORITY_HIGH) {
                SVACaptureProfile = cap_prof_priority;
            }

            // stop/unload all sva streams
            for (int i = 0; i < active_streams_st.size(); i++) {
                st_str = active_streams_st[i];
                if (st_str && isStreamActive(st_str, active_streams_st)) {
                    mResourceManagerMutex.unlock();
                    status = st_str->HandleConcurrentStream(false);
                    mResourceManagerMutex.lock();
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to stop/unload SVA stream");
                    }
                }
            }

            // load/start all sva streams
            for (int i = 0; i < active_streams_st.size(); i++) {
                st_str = active_streams_st[i];
                if (st_str && isStreamActive(st_str, active_streams_st)) {
                    mResourceManagerMutex.unlock();
                    status = st_str->HandleConcurrentStream(true);
                    mResourceManagerMutex.lock();
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to load/start SVA stream");
                    }
                }
            }
        }
    }
exit:
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit, status %d", status);
}

std::shared_ptr<Device> ResourceManager::getActiveEchoReferenceRxDevices_l(
    Stream *tx_str)
{
    int status = 0;
    int deviceId = 0;
    std::shared_ptr<Device> rx_device = nullptr;
    std::shared_ptr<Device> tx_device = nullptr;
    struct pal_stream_attributes tx_attr;
    struct pal_stream_attributes rx_attr;
    std::vector <std::shared_ptr<Device>> tx_device_list;
    std::vector <std::shared_ptr<Device>> rx_device_list;

    // check stream direction
    status = tx_str->getStreamAttributes(&tx_attr);
    if (status) {
        PAL_ERR(LOG_TAG, "stream get attributes failed");
        goto exit;
    }
    if (tx_attr.direction != PAL_AUDIO_INPUT) {
        PAL_ERR(LOG_TAG, "invalid stream direction %d", tx_attr.direction);
        status = -EINVAL;
        goto exit;
    }

    // get associated device list
    status = tx_str->getAssociatedDevices(tx_device_list);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get associated device, status %d", status);
        goto exit;
    }

    for (auto& rx_str: mActiveStreams) {
        rx_str->getStreamAttributes(&rx_attr);
        rx_device_list.clear();
        if (rx_attr.direction != PAL_AUDIO_INPUT) {
            if (!getEcRefStatus(tx_attr.type, rx_attr.type)) {
                PAL_DBG(LOG_TAG, "No need to enable ec ref for rx %d tx %d",
                        rx_attr.type, tx_attr.type);
                continue;
            }
            rx_str->getAssociatedDevices(rx_device_list);
            for (int i = 0; i < rx_device_list.size(); i++) {
                if (!isDeviceActive_l(rx_device_list[i], rx_str))
                    continue;
                deviceId = rx_device_list[i]->getSndDeviceId();
                if (deviceId > PAL_DEVICE_OUT_MIN &&
                    deviceId < PAL_DEVICE_OUT_MAX)
                    rx_device = rx_device_list[i];
                else
                    rx_device = nullptr;
                for (int j = 0; j < tx_device_list.size(); j++) {
                    tx_device = tx_device_list[j];
                    if (checkECRef(rx_device, tx_device))
                        goto exit;
                }
            }
            rx_device = nullptr;
        } else {
            continue;
        }
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return rx_device;
}

std::shared_ptr<Device> ResourceManager::getActiveEchoReferenceRxDevices(
    Stream *tx_str)
{
    std::shared_ptr<Device> rx_device = nullptr;
    PAL_DBG(LOG_TAG, "Enter.");
    mResourceManagerMutex.lock();
    rx_device = getActiveEchoReferenceRxDevices_l(tx_str);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit.");
    return rx_device;
}

std::vector<Stream*> ResourceManager::getConcurrentTxStream_l(
    Stream *rx_str,
    std::shared_ptr<Device> rx_device)
{
    int deviceId = 0;
    int status = 0;
    std::vector<Stream*> tx_stream_list;
    struct pal_stream_attributes tx_attr;
    struct pal_stream_attributes rx_attr;
    std::shared_ptr<Device> tx_device = nullptr;
    std::vector <std::shared_ptr<Device>> tx_device_list;

    // check stream direction
    status = rx_str->getStreamAttributes(&rx_attr);
    if (status) {
        PAL_ERR(LOG_TAG, "stream get attributes failed");
        goto exit;
    }
    if (rx_attr.direction != PAL_AUDIO_OUTPUT) {
        PAL_ERR(LOG_TAG, "Invalid stream direction %d", rx_attr.direction);
        status = -EINVAL;
        goto exit;
    }

    for (auto& tx_str: mActiveStreams) {
        tx_device_list.clear();
        tx_str->getStreamAttributes(&tx_attr);
        if (tx_attr.direction == PAL_AUDIO_INPUT) {
            if (!getEcRefStatus(tx_attr.type, rx_attr.type)) {
                PAL_DBG(LOG_TAG, "No need to enable ec ref for rx %d tx %d",
                        rx_attr.type, tx_attr.type);
                continue;
            }
            tx_str->getAssociatedDevices(tx_device_list);
            for (int i = 0; i < tx_device_list.size(); i++) {
                if (!isDeviceActive_l(tx_device_list[i], tx_str))
                    continue;
                deviceId = tx_device_list[i]->getSndDeviceId();
                if (deviceId > PAL_DEVICE_IN_MIN &&
                    deviceId < PAL_DEVICE_IN_MAX)
                    tx_device = tx_device_list[i];
                else
                    tx_device = nullptr;

                if (checkECRef(rx_device, tx_device)) {
                    tx_stream_list.push_back(tx_str);
                    break;
                }
            }
        }
    }
exit:
    return tx_stream_list;
}

std::vector<Stream*> ResourceManager::getConcurrentTxStream(
    Stream *rx_str,
    std::shared_ptr<Device> rx_device)
{
    std::vector<Stream*> tx_stream_list;
    PAL_DBG(LOG_TAG, "Enter.");
    mResourceManagerMutex.lock();
    tx_stream_list = getConcurrentTxStream_l(rx_str, rx_device);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit.");
    return tx_stream_list;
}

bool ResourceManager::checkECRef(std::shared_ptr<Device> rx_dev,
                                 std::shared_ptr<Device> tx_dev)
{
    bool result = false;
    int rx_dev_id = 0;
    int tx_dev_id = 0;

    if (!rx_dev || !tx_dev)
        return result;

    rx_dev_id = rx_dev->getSndDeviceId();
    tx_dev_id = tx_dev->getSndDeviceId();

    for (int i = 0; i < deviceInfo.size(); i++) {
        if (tx_dev_id == deviceInfo[i].deviceId) {
            for (int j = 0; j < deviceInfo[i].rx_dev_ids.size(); j++) {
                if (rx_dev_id == deviceInfo[i].rx_dev_ids[j]) {
                    result = true;
                    break;
                }
            }
        }
        if (result)
            break;
    }

    PAL_DBG(LOG_TAG, "EC Ref: %d, rx dev: %d, tx dev: %d",
        result, rx_dev_id, tx_dev_id);

    return result;
}

//TBD: test this piece later, for concurrency
#if 1
template <class T>
void ResourceManager::getHigherPriorityActiveStreams(const int inComingStreamPriority, std::vector<Stream*> &activestreams,
                      std::vector<T> sourcestreams)
{
    int existingStreamPriority = 0;
    pal_stream_attributes sAttr;


    typename std::vector<T>::iterator iter = sourcestreams.begin();


    for(iter; iter != sourcestreams.end(); iter++) {
        (*iter)->getStreamAttributes(&sAttr);

        existingStreamPriority = getStreamAttrPriority(&sAttr);
        if (existingStreamPriority > inComingStreamPriority)
        {
            activestreams.push_back(*iter);
        }
    }
}
#endif


template <class T>
void getActiveStreams(std::shared_ptr<Device> d, std::vector<Stream*> &activestreams,
                      std::vector<T> sourcestreams)
{
    for(typename std::vector<T>::iterator iter = sourcestreams.begin();
                 iter != sourcestreams.end(); iter++) {
        std::vector <std::shared_ptr<Device>> devices;
        (*iter)->getAssociatedDevices(devices);
        typename std::vector<std::shared_ptr<Device>>::iterator result =
                 std::find(devices.begin(), devices.end(), d);
        if (result != devices.end())
            activestreams.push_back(*iter);
    }
}

int ResourceManager::getActiveStream_l(std::shared_ptr<Device> d,
                                     std::vector<Stream*> &activestreams)
{
    int ret = 0;
    // merge all types of active streams into activestreams
    getActiveStreams(d, activestreams, active_streams_ll);
    getActiveStreams(d, activestreams, active_streams_ull);
    getActiveStreams(d, activestreams, active_streams_ulla);
    getActiveStreams(d, activestreams, active_streams_db);
    getActiveStreams(d, activestreams, active_streams_comp);
    getActiveStreams(d, activestreams, active_streams_st);
    getActiveStreams(d, activestreams, active_streams_po);
    getActiveStreams(d, activestreams, active_streams_proxy);
    getActiveStreams(d, activestreams, active_streams_incall_record);
    getActiveStreams(d, activestreams, active_streams_non_tunnel);
    getActiveStreams(d, activestreams, active_streams_incall_music);

    if (activestreams.empty()) {
        ret = -ENOENT;
        PAL_INFO(LOG_TAG, "no active streams found for device %d ret %d", d->getSndDeviceId(), ret);
    }

    return ret;
}

int ResourceManager::getActiveStream(std::shared_ptr<Device> d,
                                     std::vector<Stream*> &activestreams)
{
    int ret = 0;
    PAL_DBG(LOG_TAG, "Enter.");
    mResourceManagerMutex.lock();
    ret = getActiveStream_l(d, activestreams);
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

/*blsUpdated - to specify if the config is updated by rm*/
int ResourceManager::checkAndGetDeviceConfig(struct pal_device *device, bool* blsUpdated)
{
    int ret = -EINVAL;
    if (!device || !blsUpdated) {
        PAL_ERR(LOG_TAG, "Invalid input parameter ret %d", ret);
        return ret;
    }
    //TODO:check if device config is supported
    bool dev_supported = false;
    *blsUpdated = false;
    uint16_t channels = device->config.ch_info.channels;
    uint32_t samplerate = device->config.sample_rate;
    uint32_t bitwidth = device->config.bit_width;

    PAL_DBG(LOG_TAG, "Enter.");
    //TODO: check and rewrite params if needed
    // only compare with default value for now
    // because no config file parsed in init
    if (channels != DEFAULT_CHANNELS) {
        if (bOverwriteFlag) {
            device->config.ch_info.channels = DEFAULT_CHANNELS;
            *blsUpdated = true;
        }
    } else if (samplerate != DEFAULT_SAMPLE_RATE) {
        if (bOverwriteFlag) {
            device->config.sample_rate = DEFAULT_SAMPLE_RATE;
            *blsUpdated = true;
        }
    } else if (bitwidth != DEFAULT_BIT_WIDTH) {
        if (bOverwriteFlag) {
            device->config.bit_width = DEFAULT_BIT_WIDTH;
            *blsUpdated = true;
        }
    } else {
        ret = 0;
        dev_supported = true;
    }
    PAL_DBG(LOG_TAG, "Exit. ret %d", ret);
    return ret;
}

std::shared_ptr<ResourceManager> ResourceManager::getInstance()
{
    PAL_DBG(LOG_TAG, "Enter.");
    if(!rm) {
        std::lock_guard<std::mutex> lock(ResourceManager::mResourceManagerMutex);
        if (!rm) {
            std::shared_ptr<ResourceManager> sp(new ResourceManager());
            rm = sp;
        }
    }
    PAL_DBG(LOG_TAG, "Exit.");
    return rm;
}

int ResourceManager::getSndCard()
{
    return snd_card;
}

int ResourceManager::getSndDeviceName(int deviceId, char *device_name)
{
    if (isValidDevId(deviceId)) {
        strlcpy(device_name, sndDeviceNameLUT[deviceId].second.c_str(), DEVICE_NAME_MAX_SIZE);
    } else {
        strlcpy(device_name, "", DEVICE_NAME_MAX_SIZE);
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
        return -EINVAL;
    }
    return 0;
}

int ResourceManager::getDeviceEpName(int deviceId, std::string &epName)
{
    if (isValidDevId(deviceId)) {
        epName.assign(deviceLinkName[deviceId].second);
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
        return -EINVAL;
    }
    return 0;
}

// TODO: Should pcm device be related to usecases used(ll/db/comp/ulla)?
// Use Low Latency as default by now
int ResourceManager::getPcmDeviceId(int deviceId)
{
    int pcm_device_id = -1;
    if (!isValidDevId(deviceId)) {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
        return -EINVAL;
    }

    pcm_device_id = devicePcmId[deviceId].second;
    return pcm_device_id;
}

void ResourceManager::deinit()
{
    card_status_t state = CARD_STATUS_NONE;

    mixerClosed = true;
    mixer_close(audio_mixer);
    if (audio_route) {
       audio_route_free(audio_route);
    }
    if (mixerEventTread.joinable()) {
        mixerEventTread.join();
    }
    PAL_DBG(LOG_TAG, "Mixer event thread joined");
    if (sndmon)
        delete sndmon;

    cvMutex.lock();
    msgQ.push(state);
    cvMutex.unlock();
    cv.notify_all();

    workerThread.join();
    while (!msgQ.empty())
        msgQ.pop();

    rm = nullptr;
}

int ResourceManager::getStreamTag(std::vector <int> &tag)
{
    int status = 0;
    for (int i=0; i < streamTag.size(); i++) {
        tag.push_back(streamTag[i]);
    }
    return status;
}

int ResourceManager::getStreamPpTag(std::vector <int> &tag)
{
    int status = 0;
    for (int i=0; i < streamPpTag.size(); i++) {
        tag.push_back(streamPpTag[i]);
    }
    return status;
}

int ResourceManager::getMixerTag(std::vector <int> &tag)
{
    int status = 0;
    for (int i=0; i < mixerTag.size(); i++) {
        tag.push_back(mixerTag[i]);
    }
    return status;
}

int ResourceManager::getDeviceTag(std::vector <int> &tag)
{
    int status = 0;
    for (int i=0; i < deviceTag.size(); i++) {
        tag.push_back(deviceTag[i]);
    }
    return status;
}

int ResourceManager::getDevicePpTag(std::vector <int> &tag)
{
    int status = 0;
    for (int i=0; i < devicePpTag.size(); i++) {
        tag.push_back(devicePpTag[i]);
    }
    return status;
}

int ResourceManager::getNumFEs(const pal_stream_type_t sType) const
{
    int n = 1;

    switch (sType) {
        case PAL_STREAM_LOOPBACK:
        case PAL_STREAM_TRANSCODE:
            n = 1;
            break;
        default:
            n = 1;
            break;
    }

    return n;
}

const std::vector<int> ResourceManager::allocateFrontEndIds(const struct pal_stream_attributes sAttr, int lDirection)
{
    //TODO: lock resource manager
    std::vector<int> f;
    f.clear();
    const int howMany = getNumFEs(sAttr.type);
    int id = 0;
    std::vector<int>::iterator it;

    switch(sAttr.type) {
        case PAL_STREAM_NON_TUNNEL:
            if (howMany > listAllNonTunnelSessionIds.size()) {
                PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                howMany, listAllPcmRecordFrontEnds.size());
                 goto error;
            }
            id = (listAllNonTunnelSessionIds.size() - 1);
            it =  (listAllNonTunnelSessionIds.begin() + id);
            for (int i = 0; i < howMany; i++) {
                f.push_back(listAllNonTunnelSessionIds.at(id));
                listAllNonTunnelSessionIds.erase(it);
                PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                it -= 1;
                id -= 1;
            }
            break;
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_ULTRA_LOW_LATENCY:
        case PAL_STREAM_GENERIC:
        case PAL_STREAM_DEEP_BUFFER:
        case PAL_STREAM_VOIP:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
        case PAL_STREAM_VOICE_UI:
        case PAL_STREAM_PCM_OFFLOAD:
        case PAL_STREAM_LOOPBACK:
        case PAL_STREAM_PROXY:
            switch (sAttr.direction) {
                case PAL_AUDIO_INPUT:
                    if ( howMany > listAllPcmRecordFrontEnds.size()) {
                        PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                          howMany, listAllPcmRecordFrontEnds.size());
                        goto error;
                    }
                    id = (listAllPcmRecordFrontEnds.size() - 1);
                    it =  (listAllPcmRecordFrontEnds.begin() + id);
                    for (int i = 0; i < howMany; i++) {
                        f.push_back(listAllPcmRecordFrontEnds.at(id));
                        listAllPcmRecordFrontEnds.erase(it);
                        PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                        it -= 1;
                        id -= 1;
                    }
                    break;
                case PAL_AUDIO_OUTPUT:
                    if ( howMany > listAllPcmPlaybackFrontEnds.size()) {
                        PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                          howMany, listAllPcmPlaybackFrontEnds.size());
                        goto error;
                    }
                    id = (listAllPcmPlaybackFrontEnds.size() - 1);
                    it =  (listAllPcmPlaybackFrontEnds.begin() + id);
                    for (int i = 0; i < howMany; i++) {
                        f.push_back(listAllPcmPlaybackFrontEnds.at(id));
                        listAllPcmPlaybackFrontEnds.erase(it);
                        PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                        it -= 1;
                        id -= 1;
                    }
                    break;
                case PAL_AUDIO_INPUT | PAL_AUDIO_OUTPUT:
                    if (lDirection == RXLOOPBACK) {
                        if ( howMany > listAllPcmLoopbackRxFrontEnds.size()) {
                            PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                              howMany, listAllPcmLoopbackRxFrontEnds.size());
                            goto error;
                        }
                        id = (listAllPcmLoopbackRxFrontEnds.size() - 1);
                        it =  (listAllPcmLoopbackRxFrontEnds.begin() + id);
                        for (int i = 0; i < howMany; i++) {
                           f.push_back(listAllPcmLoopbackRxFrontEnds.at(id));
                           listAllPcmLoopbackRxFrontEnds.erase(it);
                           PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                           it -= 1;
                           id -= 1;
                        }
                    } else {
                        if ( howMany > listAllPcmLoopbackTxFrontEnds.size()) {
                            PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                              howMany, listAllPcmLoopbackTxFrontEnds.size());
                            goto error;
                        }
                        id = (listAllPcmLoopbackTxFrontEnds.size() - 1);
                        it =  (listAllPcmLoopbackTxFrontEnds.begin() + id);
                        for (int i = 0; i < howMany; i++) {
                           f.push_back(listAllPcmLoopbackTxFrontEnds.at(id));
                           listAllPcmLoopbackTxFrontEnds.erase(it);
                           PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                           it -= 1;
                           id -= 1;
                        }
                    }
                    break;
                default:
                    PAL_ERR(LOG_TAG,"direction unsupported");
                    break;
            }
            break;
        case PAL_STREAM_COMPRESSED:
            switch (sAttr.direction) {
                case PAL_AUDIO_INPUT:
                    if ( howMany > listAllCompressRecordFrontEnds.size()) {
                        PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                          howMany, listAllCompressRecordFrontEnds.size());
                        goto error;
                    }
                    id = (listAllCompressRecordFrontEnds.size() - 1);
                    it =  (listAllCompressRecordFrontEnds.begin() + id);
                    for (int i = 0; i < howMany; i++) {
                        f.push_back(listAllCompressRecordFrontEnds.at(id));
                        listAllCompressRecordFrontEnds.erase(it);
                        PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                        it -= 1;
                        id -= 1;
                    }
                    break;
                case PAL_AUDIO_OUTPUT:
                    if ( howMany > listAllCompressPlaybackFrontEnds.size()) {
                        PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                          howMany, listAllCompressPlaybackFrontEnds.size());
                        goto error;
                    }
                    id = (listAllCompressPlaybackFrontEnds.size() - 1);
                    it =  (listAllCompressPlaybackFrontEnds.begin() + id);
                    for (int i = 0; i < howMany; i++) {
                        f.push_back(listAllCompressPlaybackFrontEnds.at(id));
                        listAllCompressPlaybackFrontEnds.erase(it);
                        PAL_INFO(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                        it -= 1;
                        id -= 1;
                    }
                    break;
                default:
                    PAL_ERR(LOG_TAG,"direction unsupported");
                    break;
                }
                break;
        case PAL_STREAM_VOICE_CALL:
            switch (sAttr.direction) {
              case PAL_AUDIO_INPUT | PAL_AUDIO_OUTPUT:
                    if (lDirection == RXLOOPBACK) {
                        if (sAttr.info.voice_call_info.VSID == VOICEMMODE1 ||
                            sAttr.info.voice_call_info.VSID == VOICELBMMODE1) {
                            f = allocateVoiceFrontEndIds(listAllPcmVoice1RxFrontEnds, howMany);
                        } else if(sAttr.info.voice_call_info.VSID == VOICEMMODE2 ||
                            sAttr.info.voice_call_info.VSID == VOICELBMMODE2){
                            f = allocateVoiceFrontEndIds(listAllPcmVoice2RxFrontEnds, howMany);
                        } else {
                            PAL_ERR(LOG_TAG,"invalid VSID 0x%x provided",
                                    sAttr.info.voice_call_info.VSID);
                        }
                    } else {
                        if (sAttr.info.voice_call_info.VSID == VOICEMMODE1 ||
                            sAttr.info.voice_call_info.VSID == VOICELBMMODE1) {
                            f = allocateVoiceFrontEndIds(listAllPcmVoice1TxFrontEnds, howMany);
                        } else if(sAttr.info.voice_call_info.VSID == VOICEMMODE2 ||
                            sAttr.info.voice_call_info.VSID == VOICELBMMODE2){
                            f = allocateVoiceFrontEndIds(listAllPcmVoice2TxFrontEnds, howMany);
                        } else {
                            PAL_ERR(LOG_TAG,"invalid VSID 0x%x provided",
                                    sAttr.info.voice_call_info.VSID);
                        }
                    }
                    break;
              default:
                  PAL_ERR(LOG_TAG,"direction unsupported voice must be RX and TX");
                  break;
            }
            break;
        case PAL_STREAM_VOICE_CALL_RECORD:
            if ( howMany > listAllPcmInCallRecordFrontEnds.size()) {
                    PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                      howMany, listAllPcmInCallRecordFrontEnds.size());
                    goto error;
                }
            id = (listAllPcmInCallRecordFrontEnds.size() - 1);
            it =  (listAllPcmInCallRecordFrontEnds.begin() + id);
            for (int i = 0; i < howMany; i++) {
                f.push_back(listAllPcmInCallRecordFrontEnds.at(id));
                listAllPcmInCallRecordFrontEnds.erase(it);
                PAL_ERR(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                it -= 1;
                id -= 1;
            }
            break;
        case PAL_STREAM_VOICE_CALL_MUSIC:
            if ( howMany > listAllPcmInCallMusicFrontEnds.size()) {
                    PAL_ERR(LOG_TAG, "allocateFrontEndIds: requested for %d front ends, have only %zu error",
                                      howMany, listAllPcmInCallMusicFrontEnds.size());
                    goto error;
                }
            id = (listAllPcmInCallMusicFrontEnds.size() - 1);
            it =  (listAllPcmInCallMusicFrontEnds.begin() + id);
            for (int i = 0; i < howMany; i++) {
                f.push_back(listAllPcmInCallMusicFrontEnds.at(id));
                listAllPcmInCallMusicFrontEnds.erase(it);
                PAL_ERR(LOG_TAG, "allocateFrontEndIds: front end %d", f[i]);
                it -= 1;
                id -= 1;
            }
            break;
        default:
            break;
    }

error:
    return f;
}


const std::vector<int> ResourceManager::allocateVoiceFrontEndIds(std::vector<int> listAllPcmVoiceFrontEnds, const int howMany)
{
    std::vector<int> f;
    f.clear();
    int id = 0;
    std::vector<int>::iterator it;
    if ( howMany > listAllPcmVoiceFrontEnds.size()) {
        PAL_ERR(LOG_TAG, "allocate voice FrontEndIds: requested for %d front ends, have only %zu error",
                howMany, listAllPcmVoiceFrontEnds.size());
        return f;
    }
    id = (listAllPcmVoiceFrontEnds.size() - 1);
    it =  (listAllPcmVoiceFrontEnds.begin() + id);
    for (int i = 0; i < howMany; i++) {
        f.push_back(listAllPcmVoiceFrontEnds.at(id));
        listAllPcmVoiceFrontEnds.erase(it);
        PAL_INFO(LOG_TAG, "allocate VoiceFrontEndIds: front end %d", f[i]);
        it -= 1;
        id -= 1;
    }

    return f;
}
void ResourceManager::freeFrontEndIds(const std::vector<int> frontend,
                                      const struct pal_stream_attributes sAttr,
                                      int lDirection)
{
    PAL_INFO(LOG_TAG, "stream type %d, freeing %d\n", sAttr.type,
             frontend.at(0));

    switch(sAttr.type) {
        case PAL_STREAM_NON_TUNNEL:
            for (int i = 0; i < frontend.size(); i++) {
                 listAllNonTunnelSessionIds.push_back(frontend.at(i));
            }
            break;
        case PAL_STREAM_LOW_LATENCY:
        case PAL_STREAM_ULTRA_LOW_LATENCY:
        case PAL_STREAM_GENERIC:
        case PAL_STREAM_PROXY:
        case PAL_STREAM_DEEP_BUFFER:
        case PAL_STREAM_VOIP:
        case PAL_STREAM_VOIP_RX:
        case PAL_STREAM_VOIP_TX:
        case PAL_STREAM_VOICE_UI:
        case PAL_STREAM_PCM_OFFLOAD:
            switch (sAttr.direction) {
                case PAL_AUDIO_INPUT:
                    for (int i = 0; i < frontend.size(); i++) {
                        listAllPcmRecordFrontEnds.push_back(frontend.at(i));
                    }
                    break;
                case PAL_AUDIO_OUTPUT:
                    for (int i = 0; i < frontend.size(); i++) {
                        listAllPcmPlaybackFrontEnds.push_back(frontend.at(i));
                    }
                    break;
                case PAL_AUDIO_INPUT | PAL_AUDIO_OUTPUT:
                    if (lDirection == RXLOOPBACK) {
                        for (int i = 0; i < frontend.size(); i++) {
                            listAllPcmLoopbackRxFrontEnds.push_back(frontend.at(i));
                        }
                    } else {
                        for (int i = 0; i < frontend.size(); i++) {
                            listAllPcmLoopbackTxFrontEnds.push_back(frontend.at(i));
                        }
                    }
                    break;
                default:
                    PAL_ERR(LOG_TAG,"direction unsupported");
                    break;
            }
            break;

        case PAL_STREAM_VOICE_CALL:
            if (lDirection == RXLOOPBACK) {
                for (int i = 0; i < frontend.size(); i++) {
                    if (sAttr.info.voice_call_info.VSID == VOICEMMODE1 ||
                        sAttr.info.voice_call_info.VSID == VOICELBMMODE1) {
                        listAllPcmVoice1RxFrontEnds.push_back(frontend.at(i));
                    } else {
                        listAllPcmVoice2RxFrontEnds.push_back(frontend.at(i));
                    }

                }
            } else {
                for (int i = 0; i < frontend.size(); i++) {
                    if (sAttr.info.voice_call_info.VSID == VOICEMMODE1 ||
                        sAttr.info.voice_call_info.VSID == VOICELBMMODE1) {
                        listAllPcmVoice1TxFrontEnds.push_back(frontend.at(i));
                    } else {
                        listAllPcmVoice2TxFrontEnds.push_back(frontend.at(i));
                    }
                }
            }
            break;

        case PAL_STREAM_COMPRESSED:
            switch (sAttr.direction) {
                case PAL_AUDIO_INPUT:
                    for (int i = 0; i < frontend.size(); i++) {
                        listAllCompressRecordFrontEnds.push_back(frontend.at(i));
                    }
                    break;
                case PAL_AUDIO_OUTPUT:
                    for (int i = 0; i < frontend.size(); i++) {
                        listAllCompressPlaybackFrontEnds.push_back(frontend.at(i));
                    }
                    break;
                default:
                    PAL_ERR(LOG_TAG,"direction unsupported");
                    break;
                }
            break;
        case PAL_STREAM_VOICE_CALL_RECORD:
        case PAL_STREAM_VOICE_CALL_MUSIC:
            switch (sAttr.direction) {
              case PAL_AUDIO_INPUT:
                for (int i = 0; i < frontend.size(); i++) {
                    listAllPcmInCallRecordFrontEnds.push_back(frontend.at(i));
                }
                break;
              case PAL_AUDIO_OUTPUT:
                for (int i = 0; i < frontend.size(); i++) {
                    listAllPcmInCallMusicFrontEnds.push_back(frontend.at(i));
                }
                break;
              default:
                break;
            }
            break;
        default:
            break;
    }
    return;
}

void ResourceManager::getSharedBEActiveStreamDevs(std::vector <std::tuple<Stream *, uint32_t>> &activeStreamsDevices,
                                                  int dev_id)
{
    std::string backEndName;
    std::shared_ptr<Device> dev;
    std::vector <Stream *> activeStreams;

    if (isValidDevId(dev_id) && (dev_id != PAL_DEVICE_NONE))
        backEndName = listAllBackEndIds[dev_id].second;
    for (int i = PAL_DEVICE_OUT_MIN; i < PAL_DEVICE_IN_MAX; i++) {
        if (backEndName == listAllBackEndIds[i].second) {
            dev = Device::getObject((pal_device_id_t) i);
            if(dev) {
                getActiveStream_l(dev, activeStreams);
                PAL_DBG(LOG_TAG, "got dev %d active streams on dev is %zu", i, activeStreams.size() );
                for (int j=0; j < activeStreams.size(); j++) {
                    activeStreamsDevices.push_back({activeStreams[j], i});
                    PAL_DBG(LOG_TAG, "found shared BE stream %pK with dev %d", activeStreams[j], i );
                }
            }
            activeStreams.clear();
        }
    }
}

const std::vector<std::string> ResourceManager::getBackEndNames(
        const std::vector<std::shared_ptr<Device>> &deviceList) const
{
    std::vector<std::string> backEndNames;
    std::string epname;
    backEndNames.clear();

    int dev_id;

    for (int i = 0; i < deviceList.size(); i++) {
        dev_id = deviceList[i]->getSndDeviceId();
        PAL_ERR(LOG_TAG, "device id %d", dev_id);
        if (isValidDevId(dev_id)) {
            epname.assign(listAllBackEndIds[dev_id].second);
            backEndNames.push_back(epname);
        } else {
            PAL_ERR(LOG_TAG, "Invalid device id %d", dev_id);
        }
    }

    for (int i = 0; i < backEndNames.size(); i++) {
        PAL_ERR(LOG_TAG, "getBackEndNames: going to return %s", backEndNames[i].c_str());
    }

    return backEndNames;
}

void ResourceManager::getBackEndNames(
        const std::vector<std::shared_ptr<Device>> &deviceList,
        std::vector<std::pair<int32_t, std::string>> &rxBackEndNames,
        std::vector<std::pair<int32_t, std::string>> &txBackEndNames) const
{
    std::string epname;
    rxBackEndNames.clear();
    txBackEndNames.clear();

    int dev_id;

    for (int i = 0; i < deviceList.size(); i++) {
        dev_id = deviceList[i]->getSndDeviceId();
        if (dev_id > PAL_DEVICE_OUT_MIN && dev_id < PAL_DEVICE_OUT_MAX) {
            epname.assign(listAllBackEndIds[dev_id].second);
            rxBackEndNames.push_back(std::make_pair(dev_id, epname));
        } else if (dev_id > PAL_DEVICE_IN_MIN && dev_id < PAL_DEVICE_IN_MAX) {
            epname.assign(listAllBackEndIds[dev_id].second);
            txBackEndNames.push_back(std::make_pair(dev_id, epname));
        } else {
            PAL_ERR(LOG_TAG, "Invalid device id %d", dev_id);
        }
    }

    for (int i = 0; i < rxBackEndNames.size(); i++)
        PAL_DBG(LOG_TAG, "getBackEndNames (RX): %s", rxBackEndNames[i].second.c_str());
    for (int i = 0; i < txBackEndNames.size(); i++)
        PAL_DBG(LOG_TAG, "getBackEndNames (TX): %s", txBackEndNames[i].second.c_str());
}
#if 0
const bool ResourceManager::shouldDeviceSwitch(const pal_stream_attributes* sExistingAttr,
    const pal_stream_attributes* sIncomingAttr) const {

    bool dSwitch = false;
    int existingPriority = 0;
    int incomingPriority = 0;
    bool ifVoice

    if (!sExistingAttr || !sIncomingAttr)
        goto error;

    existingPriority = getStreamAttrPriority(existingStream->getStreamAttributes(&sExistingAttr));
    incomingPriority = getStreamAttrPriority(incomingStream->getStreamAttributes(&sIncomingAttr));

    dSwitch = (incomingPriority > existingPriority);

    PAL_VERBOSE(LOG_TAG, "should Device switch or not %d, incoming Stream priority %d, existing stream priority %d",
        dSwitch, incomingPriority, existingPriority);

error:
    return dSwitch;
}
#endif

bool ResourceManager::isDeviceSwitchRequired(struct pal_device *activeDevAttr,
         struct pal_device *inDevAttr, const pal_stream_attributes* inStrAttr)
{
    bool is_ds_required = false;
    /*  This API may need stream attributes also to decide the priority like voice call has high priority */
    /* Right now assume all playback streams are same priority and decide based on Active Device config */

    if (!activeDevAttr || !inDevAttr || !inStrAttr) {
        PAL_ERR(LOG_TAG, "Invalid input parameter ");
        return is_ds_required;
    }

    switch (inDevAttr->id) {
    /* speaker is always at 48k, 16 bit, 2 ch */
    case PAL_DEVICE_OUT_SPEAKER:
        is_ds_required = false;
        break;
    case PAL_DEVICE_OUT_USB_HEADSET:
    case PAL_DEVICE_OUT_USB_DEVICE:
        if ((activeDevAttr->config.sample_rate == SAMPLINGRATE_44K) &&
            (inStrAttr->type == PAL_STREAM_LOW_LATENCY) ) {
            PAL_INFO(LOG_TAG, "active stream is at 44.1kHz.");
            is_ds_required = false;
        } else if ((PAL_AUDIO_OUTPUT == inStrAttr->direction) &&
            (inDevAttr->config.sample_rate % SAMPLINGRATE_44K == 0)) {
            //Native Audio usecase
            PAL_ERR(LOG_TAG, "1 inDevAttr->config.sample_rate = %d  ", inDevAttr->config.sample_rate);
            is_ds_required = true;
        } else if ((activeDevAttr->config.sample_rate < inDevAttr->config.sample_rate) ||
            (activeDevAttr->config.bit_width < inDevAttr->config.bit_width) ||
            (activeDevAttr->config.ch_info.channels < inDevAttr->config.ch_info.channels)) {
            is_ds_required = true;
        }
        break;
    case PAL_DEVICE_OUT_WIRED_HEADSET:
    case PAL_DEVICE_OUT_WIRED_HEADPHONE:
        if ((PAL_STREAM_VOICE_CALL == inStrAttr->type) && ((activeDevAttr->config.sample_rate != inDevAttr->config.sample_rate) ||
            (activeDevAttr->config.bit_width != inDevAttr->config.bit_width) ||
            (activeDevAttr->config.ch_info.channels != inDevAttr->config.ch_info.channels))) {
            is_ds_required = true;
        } else if ((PAL_STREAM_COMPRESSED == inStrAttr->type || PAL_STREAM_PCM_OFFLOAD == inStrAttr->type) &&
            (NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_DSP == getNativeAudioSupport()) &&
            (PAL_AUDIO_OUTPUT == inStrAttr->direction) &&
            (inStrAttr->out_media_config.sample_rate % SAMPLINGRATE_44K == 0)) {

            //Native Audio usecase
            if (activeDevAttr->config.sample_rate != inStrAttr->out_media_config.sample_rate) {
                inDevAttr->config.sample_rate = inStrAttr->out_media_config.sample_rate;
                is_ds_required = true;
            }
        } else if ((activeDevAttr->config.sample_rate < inDevAttr->config.sample_rate) ||
            (activeDevAttr->config.bit_width < inDevAttr->config.bit_width) ||
            (activeDevAttr->config.ch_info.channels < inDevAttr->config.ch_info.channels)) {
            is_ds_required = true;
        }
        break;
    case PAL_DEVICE_OUT_AUX_DIGITAL:
    case PAL_DEVICE_OUT_AUX_DIGITAL_1:
    case PAL_DEVICE_OUT_HDMI:
        if (activeDevAttr->config.ch_info.channels < inDevAttr->config.ch_info.channels)
            is_ds_required = true;
        else if ((activeDevAttr->config.sample_rate < inDevAttr->config.sample_rate) ||
                (activeDevAttr->config.bit_width < inDevAttr->config.bit_width)) {
            is_ds_required = true;
        } else if ((PAL_STREAM_COMPRESSED == inStrAttr->type ||
                    PAL_STREAM_PCM_OFFLOAD == inStrAttr->type) &&
            (PAL_AUDIO_OUTPUT == inStrAttr->direction) &&
            (inStrAttr->out_media_config.sample_rate % SAMPLINGRATE_44K == 0)) {

            //Native Audio usecase
            if (activeDevAttr->config.sample_rate != inStrAttr->out_media_config.sample_rate) {
                inDevAttr->config.sample_rate = inStrAttr->out_media_config.sample_rate;
                is_ds_required = true;
            }
        }
        break;
    default:
        is_ds_required = false;
        break;
    }

    return is_ds_required;
}

int32_t ResourceManager::streamDevDisconnect(std::vector <std::tuple<Stream *, uint32_t>> streamDevDisconnectList){
    int status = 0;
    std::vector <std::tuple<Stream *, uint32_t>>::iterator sIter;

    /* disconnect active list from the current devices they are attached to */
    for (sIter = streamDevDisconnectList.begin(); sIter != streamDevDisconnectList.end(); sIter++) {
        status = (std::get<0>(*sIter))->disconnectStreamDevice(std::get<0>(*sIter), (pal_device_id_t)std::get<1>(*sIter));
        if (status) {
            PAL_ERR(LOG_TAG, "failed to disconnect stream %pK from device %d",
                    std::get<0>(*sIter), std::get<1>(*sIter));
            goto error;
        } else {
            PAL_DBG(LOG_TAG, "disconnect stream %pK from device %d",
                    std::get<0>(*sIter), std::get<1>(*sIter));
        }
    }
error:
    return status;
}

int32_t ResourceManager::streamDevConnect(std::vector <std::tuple<Stream *, struct pal_device *>> streamDevConnectList){
    int status = 0;
    std::vector <std::tuple<Stream *, struct pal_device *>>::iterator sIter;

    /* connect active list from the current devices they are attached to */
    for (sIter = streamDevConnectList.begin(); sIter != streamDevConnectList.end(); sIter++) {
        status = std::get<0>(*sIter)->connectStreamDevice(std::get<0>(*sIter), std::get<1>(*sIter));
        if (status) {
            PAL_ERR(LOG_TAG,"failed to connect stream %pK from device %d",
                    std::get<0>(*sIter), (std::get<1>(*sIter))->id);
            goto error;
        } else {
            PAL_DBG(LOG_TAG,"connected stream %pK from device %d",
                    std::get<0>(*sIter), (std::get<1>(*sIter))->id);
        }
    }
error:
    return status;
}


int32_t ResourceManager::streamDevSwitch(std::vector <std::tuple<Stream *, uint32_t>> streamDevDisconnectList,
                                         std::vector <std::tuple<Stream *, struct pal_device *>> streamDevConnectList)
{
    int status = 0;

    if (cardState == CARD_STATUS_OFFLINE) {
        PAL_ERR(LOG_TAG, "Sound card offline");
        return -EINVAL;
    }
    status = streamDevDisconnect(streamDevDisconnectList);
    if (status) {
        PAL_ERR(LOG_TAG,"disconnect failed");
        goto error;
    }
    status = streamDevConnect(streamDevConnectList);
    if (status) {
        PAL_ERR(LOG_TAG,"Connect failed");
    }
error:
    return status;
}

//when returning from this function, the device config will be updated with
//the device config of the highest priority stream

//TBD: manage re-routing of existing lower priority streams if incoming
//stream is a higher priority stream. Priority defined in ResourceManager.h
//(details below)
bool ResourceManager::updateDeviceConfig(std::shared_ptr<Device> inDev,
           struct pal_device *inDevAttr, const pal_stream_attributes* inStrAttr)
{
    bool isDeviceSwitch = false;
    int status = 0;
    std::vector <std::tuple<Stream *, uint32_t>> streamDevDisconnect, sharedBEStreamDev;
    std::vector <std::tuple<Stream *, struct pal_device *>> StreamDevConnect;

    if (!inDev || !inDevAttr) {
        goto error;
    }

    //get the active streams on the device
    //if higher priority stream exists on any of the incoming device, update the config of incoming device
    //based on device config of higher priority stream

    //TBD: if incoming stream is a higher priority,
    //call callback into all streams
    //for all devices matching incoming device id
    //and route the lower priority to new device (disable session, disable device, enable session, enable device
    //return from callback

    //check if there are shared backends
    // if yes add them to streams to device switch
    getSharedBEActiveStreamDevs(sharedBEStreamDev, inDevAttr->id);
    if (sharedBEStreamDev.size() > 0) {
        for (const auto &elem : sharedBEStreamDev) {
            struct pal_stream_attributes sAttr;
            Stream *sharedStream = std::get<0>(elem);
            struct pal_device curDevAttr;
            std::shared_ptr<Device> curDev = nullptr;

            curDevAttr.id = (pal_device_id_t)std::get<1>(elem);
            curDev = Device::getInstance(&curDevAttr, rm);
            if (!curDev) {
                PAL_ERR(LOG_TAG, "Getting Device instance failed");
                continue;
            }
            curDev->getDeviceAttributes(&curDevAttr);
            sharedStream->getStreamAttributes(&sAttr);

            if ((PAL_STREAM_VOICE_CALL == sAttr.type)) {
                PAL_INFO(LOG_TAG, "Active voice stream running on %d, Force switch",
                                  curDevAttr.id);
                curDev->getDeviceAttributes(inDevAttr);
                continue;
            }

            /* If other stream is currently running on same device, then
             * check if it needs device switch. If not needed, then use the
             * same device attribute as current running device, do not
             * add it to streamDevConnect and streamDevDisconnect list and
             * continue for next streamDev.
             */
            if (curDevAttr.id == inDevAttr->id) {
                if (!rm->isDeviceSwitchRequired(&curDevAttr,
                            inDevAttr, inStrAttr)) {
                    curDev->getDeviceAttributes(inDevAttr);
                    continue;
                }
            }
            isDeviceSwitch = true;
            streamDevDisconnect.push_back(elem);
            StreamDevConnect.push_back({std::get<0>(elem), inDevAttr});
        }
    }

    //if device switch is needed, perform it
    if (streamDevDisconnect.size()) {
        status = streamDevSwitch(streamDevDisconnect, StreamDevConnect);
        if (status) {
            PAL_ERR(LOG_TAG,"deviceswitch failed with %d", status);
        }
    }
    inDev->setDeviceAttributes(*inDevAttr);

error:
    return isDeviceSwitch;
}

int32_t ResourceManager::forceDeviceSwitch(std::shared_ptr<Device> inDev,
                                              struct pal_device *newDevAttr)
{
    int status = 0;
    std::vector <Stream *> activeStreams;
    std::vector <std::tuple<Stream *, uint32_t>> streamDevDisconnect;
    std::vector <std::tuple<Stream *, struct pal_device *>> StreamDevConnect;
    std::vector<Stream*>::iterator sIter;

    if (!inDev || !newDevAttr) {
        return -EINVAL;
    }

    //get the active streams on the device
    getActiveStream_l(inDev, activeStreams);
    if (activeStreams.size() == 0) {
        PAL_ERR(LOG_TAG, "no other active streams found");
        goto done;
    }
    //created dev switch vectors

    for(sIter = activeStreams.begin(); sIter != activeStreams.end(); sIter++) {
        streamDevDisconnect.push_back({(*sIter), inDev->getSndDeviceId()});
        StreamDevConnect.push_back({(*sIter), newDevAttr});
    }
    status = streamDevSwitch(streamDevDisconnect, StreamDevConnect);
    if (status) {
         PAL_ERR(LOG_TAG, "forceDeviceSwitch failed %d", status);
    }

done:
    return 0;
}

const std::string ResourceManager::getPALDeviceName(const pal_device_id_t id) const
{
    PAL_DBG(LOG_TAG, "id %d", id);
    if (isValidDevId(id)) {
        return deviceNameLUT.at(id);
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", id);
        return std::string("");
    }
}

int ResourceManager::getBackendName(int deviceId, std::string &backendName)
{
    if (isValidDevId(deviceId) && (deviceId != PAL_DEVICE_NONE)) {
        backendName.assign(listAllBackEndIds[deviceId].second);
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
        return -EINVAL;
    }
    return 0;
}

bool ResourceManager::isValidDevId(int deviceId)
{
    if (((deviceId >= PAL_DEVICE_NONE) && (deviceId < PAL_DEVICE_OUT_MAX))
        || ((deviceId > PAL_DEVICE_IN_MIN) && (deviceId < PAL_DEVICE_IN_MAX)))
        return true;

    return false;
}

bool ResourceManager::isOutputDevId(int deviceId)
{
    if ((deviceId > PAL_DEVICE_NONE) && (deviceId < PAL_DEVICE_OUT_MAX))
        return true;

    return false;
}

bool ResourceManager::isInputDevId(int deviceId)
{
    if ((deviceId > PAL_DEVICE_IN_MIN) && (deviceId < PAL_DEVICE_IN_MAX))
        return true;

    return false;
}

bool ResourceManager::matchDevDir(int devId1, int devId2)
{
    if (isOutputDevId(devId1) && isOutputDevId(devId2))
        return true;
    if (isInputDevId(devId1) && isInputDevId(devId2))
        return true;

    return false;
}

bool ResourceManager::isNonALSACodec(const struct pal_device * /*device*/) const
{

    //return false on our target, move configuration to xml

    return false;
}

bool ResourceManager::ifVoiceorVoipCall (const pal_stream_type_t streamType) const {

   bool voiceOrVoipCall = false;

   switch (streamType) {
       case PAL_STREAM_VOIP:
       case PAL_STREAM_VOIP_RX:
       case PAL_STREAM_VOIP_TX:
       case PAL_STREAM_VOICE_CALL:
           voiceOrVoipCall = true;
           break;
       default:
           voiceOrVoipCall = false;
           break;
    }

    return voiceOrVoipCall;
}

int ResourceManager::getCallPriority(bool ifVoiceCall) const {

//TBD: replace this with XML based priorities
    if (ifVoiceCall) {
        return 100;
    } else {
        return 0;
    }
}

int ResourceManager::getStreamAttrPriority (const pal_stream_attributes* sAttr) const {
    int priority = 0;

    if (!sAttr)
        goto exit;


    priority = getCallPriority(ifVoiceorVoipCall(sAttr->type));


    //44.1 or multiple or 24 bit

    if ((sAttr->in_media_config.sample_rate % 44100) == 0) {
        priority += 50;
    }

    if (sAttr->in_media_config.bit_width == 24) {
        priority += 25;
    }

exit:
    return priority;
}

int ResourceManager::getNativeAudioSupport()
{
    int ret = NATIVE_AUDIO_MODE_INVALID;
    if (na_props.rm_na_prop_enabled &&
        na_props.ui_na_prop_enabled) {
        ret = na_props.na_mode;
    }
    PAL_ERR(LOG_TAG,"napb: ui Prop enabled(%d) mode(%d)",
           na_props.ui_na_prop_enabled, na_props.na_mode);
    return ret;
}

int ResourceManager::setNativeAudioSupport(int na_mode)
{
    if (NATIVE_AUDIO_MODE_SRC == na_mode || NATIVE_AUDIO_MODE_TRUE_44_1 == na_mode
        || NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_CODEC == na_mode
        || NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_DSP == na_mode) {
        na_props.rm_na_prop_enabled = na_props.ui_na_prop_enabled = true;
        na_props.na_mode = na_mode;
        PAL_DBG(LOG_TAG,"napb: native audio playback enabled in (%s) mode",
              ((na_mode == NATIVE_AUDIO_MODE_SRC)?"SRC":
               (na_mode == NATIVE_AUDIO_MODE_TRUE_44_1)?"True":
               (na_mode == NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_CODEC)?"Multiple_Mix_Codec":"Multiple_Mix_DSP"));
    }
    else {
        na_props.rm_na_prop_enabled = false;
        na_props.na_mode = NATIVE_AUDIO_MODE_INVALID;
        PAL_VERBOSE(LOG_TAG,"napb: native audio playback disabled");
    }

    return 0;
}

void ResourceManager::getNativeAudioParams(struct str_parms *query,
                             struct str_parms *reply,
                             char *value, int len)
{
    int ret;
    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_NATIVE_AUDIO,
                            value, len);
    if (ret >= 0) {
        if (na_props.rm_na_prop_enabled) {
            str_parms_add_str(reply, AUDIO_PARAMETER_KEY_NATIVE_AUDIO,
                          na_props.ui_na_prop_enabled ? "true" : "false");
            PAL_VERBOSE(LOG_TAG,"napb: na_props.ui_na_prop_enabled: %d",
                  na_props.ui_na_prop_enabled);
        } else {
            str_parms_add_str(reply, AUDIO_PARAMETER_KEY_NATIVE_AUDIO,
                              "false");
            PAL_VERBOSE(LOG_TAG,"napb: native audio not supported: %d",
                  na_props.rm_na_prop_enabled);
        }
    }
}

int ResourceManager::setConfigParams(struct str_parms *parms)
{
    char *value=NULL;
    int len;
    int ret = 0;
    char *kv_pairs = str_parms_to_str(parms);

    if(kv_pairs == NULL) {
        ret = -ENOMEM;
        PAL_ERR(LOG_TAG," key-value pair is NULL");
        goto done;
    }

    PAL_DBG(LOG_TAG," enter: %s", kv_pairs);

    len = strlen(kv_pairs);
    value = (char*)calloc(len, sizeof(char));
    if(value == NULL) {
        ret = -ENOMEM;
        PAL_ERR(LOG_TAG,"failed to allocate memory");
        goto done;
    }
    ret = setNativeAudioParams(parms, value, len);

    ret = setLoggingLevelParams(parms, value, len);
done:
    PAL_VERBOSE(LOG_TAG," exit with code(%d)", ret);
    if(value != NULL)
        free(value);
    return ret;
}

int ResourceManager::setLoggingLevelParams(struct str_parms *parms,
                                          char *value, int len)
{
    int ret = -EINVAL;

    if (!value || !parms)
        return ret;

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_LOG_LEVEL,
                                value, len);
    if (ret >= 0) {
        pal_log_lvl = std::stoi(value,0,16);
        PAL_INFO(LOG_TAG, "pal logging level is set to 0x%x",
                 pal_log_lvl);
        ret = 0;

    }
    return ret;
}


int ResourceManager::setNativeAudioParams(struct str_parms *parms,
                                          char *value, int len)
{
    int ret = -EINVAL;
    int mode = NATIVE_AUDIO_MODE_INVALID;

    if (!value || !parms)
        return ret;

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_MAX_SESSIONS,
                                value, len);
    if (ret >= 0) {
        max_session_num = std::stoi(value);
        PAL_INFO(LOG_TAG, "Max sessions supported for each stream type are %d",
                 max_session_num);

    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_MAX_NT_SESSIONS,
                                value, len);
    if (ret >= 0) {
        max_nt_sessions = std::stoi(value);
        PAL_INFO(LOG_TAG, "Max sessions supported for NON_TUNNEL stream type are %d",
                 max_nt_sessions);

    }


    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_NATIVE_AUDIO_MODE,
                             value, len);
    PAL_VERBOSE(LOG_TAG," value %s", value);
    if (ret >= 0) {
        if (value && !strncmp(value, "src", sizeof("src")))
            mode = NATIVE_AUDIO_MODE_SRC;
        else if (value && !strncmp(value, "true", sizeof("true")))
            mode = NATIVE_AUDIO_MODE_TRUE_44_1;
        else if (value && !strncmp(value, "multiple_mix_codec", sizeof("multiple_mix_codec")))
            mode = NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_CODEC;
        else if (value && !strncmp(value, "multiple_mix_dsp", sizeof("multiple_mix_dsp")))
            mode = NATIVE_AUDIO_MODE_MULTIPLE_MIX_IN_DSP;
        else {
            mode = NATIVE_AUDIO_MODE_INVALID;
            PAL_ERR(LOG_TAG,"napb:native_audio_mode in RM xml,invalid mode(%s) string", value);
        }
        PAL_VERBOSE(LOG_TAG,"napb: updating mode (%d) from XML", mode);
        setNativeAudioSupport(mode);
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_NATIVE_AUDIO,
                             value, len);
    PAL_VERBOSE(LOG_TAG," value %s", value);
    if (ret >= 0) {
        if (na_props.rm_na_prop_enabled) {
            if (!strncmp("true", value, sizeof("true"))) {
                na_props.ui_na_prop_enabled = true;
                PAL_VERBOSE(LOG_TAG,"napb: native audio feature enabled from UI");
            } else {
                na_props.ui_na_prop_enabled = false;
                PAL_VERBOSE(LOG_TAG,"napb: native audio feature disabled from UI");
            }

            str_parms_del(parms, AUDIO_PARAMETER_KEY_NATIVE_AUDIO);
            //TO-DO
            // Update the concurrencies
        } else {
              PAL_VERBOSE(LOG_TAG,"napb: native audio cannot be enabled from UI");
        }
    }
    return ret;
}
void ResourceManager::updatePcmId(int32_t deviceId, int32_t pcmId)
{
    if (isValidDevId(deviceId)) {
        devicePcmId[deviceId].second = pcmId;
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
    }
}

void ResourceManager::updateLinkName(int32_t deviceId, std::string linkName)
{
    if (isValidDevId(deviceId)) {
        deviceLinkName[deviceId].second = linkName;
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
    }
}

void ResourceManager::updateSndName(int32_t deviceId, std::string sndName)
{
    if (isValidDevId(deviceId)) {
        sndDeviceNameLUT[deviceId].second = sndName;
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
    }
}

void ResourceManager::updateBackEndName(int32_t deviceId, std::string backEndName)
{
    if (isValidDevId(deviceId)) {
        listAllBackEndIds[deviceId].second = backEndName;
    } else {
        PAL_ERR(LOG_TAG, "Invalid device id %d", deviceId);
    }
}

int convertCharToHex(std::string num)
{
    uint64_t hexNum = 0;
    uint32_t base = 1;
    const char * charNum = num.c_str();
    int32_t len = strlen(charNum);
    for (int i = len-1; i>=2; i--) {
        if (charNum[i] >= '0' && charNum[i] <= '9') {
            hexNum += (charNum[i] - 48) * base;
            base = base << 4;
        } else if (charNum[i] >= 'A' && charNum[i] <= 'F') {
            hexNum += (charNum[i] - 55) * base;
            base = base << 4;
        } else if (charNum[i] >= 'a' && charNum[i] <= 'f') {
            hexNum += (charNum[i] - 87) * base;
            base = base << 4;
        }
    }
    return (int32_t) hexNum;
}

// must be called with mResourceManagerMutex held
int32_t ResourceManager::a2dpSuspend()
{
    std::shared_ptr<Device> dev = nullptr;
    struct pal_device dattr;
    int status = 0;
    std::vector <Stream *> activeStreams;
    std::vector<Stream*>::iterator sIter;
    struct pal_device_info devinfo = {};

    dattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
    dev = Device::getInstance(&dattr , rm);

    getActiveStream_l(dev, activeStreams);

    if (activeStreams.size() == 0) {
        PAL_ERR(LOG_TAG, "no active streams found");
        goto exit;
    }

    for(sIter = activeStreams.begin(); sIter != activeStreams.end(); sIter++) {
        int ret = 0;
        struct pal_stream_attributes sAttr = {};

        ret = (*sIter)->getStreamAttributes(&sAttr);
        if (0 != ret) {
            PAL_ERR(LOG_TAG, "getStreamType failed with status = %d", ret);
            goto exit;
        }
        if (sAttr.type == PAL_STREAM_COMPRESSED) {
            if (!((*sIter)->a2dp_compress_mute)) {
                struct pal_device speakerDattr;

                PAL_DBG(LOG_TAG, "selecting speaker and muting stream");
                (*sIter)->pause();
                (*sIter)->mute(true); // mute the stream, unmute during a2dp_resume
                (*sIter)->a2dp_compress_mute = true;
                // force switch to speaker
                speakerDattr.id = PAL_DEVICE_OUT_SPEAKER;

                getDeviceInfo(speakerDattr.id, sAttr.type, &devinfo);
                if ((devinfo.channels == 0) ||
                       (devinfo.channels > devinfo.max_channels)) {
                    status = -EINVAL;
                    PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                    goto exit;
                }
                getDeviceConfig(&speakerDattr, &sAttr, devinfo.channels);
                mResourceManagerMutex.unlock();
                forceDeviceSwitch(dev, &speakerDattr);
                mResourceManagerMutex.lock();
                (*sIter)->resume();
                /* backup actual device name in stream class */
                (*sIter)->suspendedDevId = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            }
        } else {
            // put to standby for non offload usecase
            mResourceManagerMutex.unlock();
            (*sIter)->standby();
            mResourceManagerMutex.lock();
        }
    }

exit:
    return status;
}

// must be called with mResourceManagerMutex held
int32_t ResourceManager::a2dpResume()
{
    std::shared_ptr<Device> dev = nullptr;
    struct pal_device dattr;
    int status = 0;
    std::vector <Stream *> activeStreams;
    std::vector<Stream*>::iterator sIter;
    struct pal_stream_attributes sAttr;

    dattr.id = PAL_DEVICE_OUT_SPEAKER;
    dev = Device::getInstance(&dattr , rm);

    getActiveStream_l(dev, activeStreams);

    if (activeStreams.size() == 0) {
        PAL_ERR(LOG_TAG, "no active streams found");
        goto exit;
    }

    // check all active stream associated with speaker
    // if the stream actual device is a2dp, then switch back to a2dp
    // unmute the stream
    for(sIter = activeStreams.begin(); sIter != activeStreams.end(); sIter++) {

        struct pal_device_info devinfo = {};

        status = (*sIter)->getStreamAttributes(&sAttr);
        if (0 != status) {
            PAL_ERR(LOG_TAG,"getStreamAttributes Failed \n");
            goto exit;
        }
        if (sAttr.type == PAL_STREAM_COMPRESSED &&
            ((*sIter)->suspendedDevId == PAL_DEVICE_OUT_BLUETOOTH_A2DP)) {
            struct pal_device a2dpDattr;

            a2dpDattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            PAL_DBG(LOG_TAG, "restoring A2dp and unmuting stream");

            getDeviceInfo(a2dpDattr.id, sAttr.type, &devinfo);
            if ((devinfo.channels == 0) ||
                   (devinfo.channels > devinfo.max_channels)) {
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                goto exit;
            }

            getDeviceConfig(&a2dpDattr, &sAttr, devinfo.channels);
            mResourceManagerMutex.unlock();
            forceDeviceSwitch(dev, &a2dpDattr);
            mResourceManagerMutex.lock();
            (*sIter)->suspendedDevId = PAL_DEVICE_NONE;
            if ((*sIter)->a2dp_compress_mute) {
                (*sIter)->mute(false);
                (*sIter)->a2dp_compress_mute = false;
            }
        }
    }
exit:
    return status;
}

int ResourceManager::getParameter(uint32_t param_id, void **param_payload,
                     size_t *payload_size, void *query __unused)
{
    int status = 0;

    PAL_INFO(LOG_TAG, "param_id=%d", param_id);
    mResourceManagerMutex.lock();
    switch (param_id) {
        case PAL_PARAM_ID_UIEFFECT:
        {
#if 0
            gef_payload_t *gef_payload = (gef_payload_t *)query;
            int index = 0;
            int pal_device_id = 0;
            int stream_type = 0;
            bool match = false;
            std::vector<Stream*>::iterator sIter;
            for(sIter = mActiveStreams.begin(); sIter != mActiveStreams.end(); sIter++) {
                match = (*sIter)->isGKVMatch(gef_payload->graph);
                if (match) {
                    pal_param_payload *pal_payload;
                    pal_payload.payload = (uint8_t *)&gef_payload->data;
                    status = (*sIter)->getEffectParameters((void *)&pal_payload, payload_size);
                    break;
                }
            }
#endif
            break;
        }
        case PAL_PARAM_ID_BT_A2DP_RECONFIG_SUPPORTED:
        case PAL_PARAM_ID_BT_A2DP_SUSPENDED:
        case PAL_PARAM_ID_BT_A2DP_ENCODER_LATENCY:
        {
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;
            pal_param_bta2dp_t *param_bt_a2dp = nullptr;

            dattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            if (isDeviceAvailable(dattr.id)) {
                dev = Device::getInstance(&dattr , rm);
                status = dev->getDeviceParameter(param_id, (void **)&param_bt_a2dp);
                if (status) {
                    PAL_ERR(LOG_TAG, "get Parameter %d failed\n", param_id);
                    goto exit;
                }
                *param_payload = param_bt_a2dp;
                *payload_size = sizeof(pal_param_bta2dp_t);
            }
            break;
        }
        case PAL_PARAM_ID_GAIN_LVL_MAP:
        {
            pal_param_gain_lvl_map_t *param_gain_lvl_map =
                (pal_param_gain_lvl_map_t *)param_payload;

            param_gain_lvl_map->filled_size =
                getGainLevelMapping(param_gain_lvl_map->mapping_tbl,
                                    param_gain_lvl_map->table_size);
            *payload_size = sizeof(pal_param_gain_lvl_map_t);
            break;
        }
        case PAL_PARAM_ID_DEVICE_CAPABILITY:
        {
            pal_param_device_capability_t *param_device_capability = (pal_param_device_capability_t *)(*param_payload);
            PAL_INFO(LOG_TAG, "Device %d card = %d palid=%x",
                        param_device_capability->addr.device_num,
                        param_device_capability->addr.card_id,
                        param_device_capability->id);
            status = getDeviceDefaultCapability(*param_device_capability);
            break;
        }
        case PAL_PARAM_ID_GET_SOUND_TRIGGER_PROPERTIES:
        {
            PAL_INFO(LOG_TAG, "get sound trigge properties, status %d", status);
            struct pal_st_properties *qstp =
                (struct pal_st_properties *)calloc(1, sizeof(struct pal_st_properties));

            GetVoiceUIProperties(qstp);

            *param_payload = qstp;
            *payload_size = sizeof(pal_st_properties);
            break;
        }
        case PAL_PARAM_ID_SP_MODE:
        {
            PAL_INFO(LOG_TAG, "get parameter for FTM mode");
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;
            dattr.id = PAL_DEVICE_OUT_SPEAKER;
            dev = Device::getInstance(&dattr , rm);
            if (dev) {
                *payload_size = dev->getParameter(PAL_PARAM_ID_SP_MODE,
                                    param_payload);
            }
        }
        break;
        default:
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Unknown ParamID:%d", param_id);
            break;
    }
exit:
    mResourceManagerMutex.unlock();
    return status;
}


int ResourceManager::getParameter(uint32_t param_id, void *param_payload,
                                  size_t payload_size __unused,
                                  pal_device_id_t pal_device_id,
                                  pal_stream_type_t pal_stream_type)
{
    int status = 0;

    PAL_INFO(LOG_TAG, "param_id=%d", param_id);
    mResourceManagerMutex.lock();
    switch (param_id) {
        case PAL_PARAM_ID_UIEFFECT:
        {
            bool match = false;
            std::vector<Stream*>::iterator sIter;
            for(sIter = mActiveStreams.begin(); sIter != mActiveStreams.end(); sIter++) {
                match = (*sIter)->checkStreamMatch(pal_device_id, pal_stream_type);
                if (match) {
                    status = (*sIter)->getEffectParameters(param_payload);
                    break;
                }
            }
            break;
        }
        default:
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Unknown ParamID:%d", param_id);
            break;
    }

    mResourceManagerMutex.unlock();

    return status;
}

int ResourceManager::setParameter(uint32_t param_id, void *param_payload,
                                  size_t payload_size)
{
    int status = 0;

    PAL_DBG(LOG_TAG, "Enter param id: %d", param_id);

    mResourceManagerMutex.lock();
    switch (param_id) {
        case PAL_PARAM_ID_SCREEN_STATE:
        {
            pal_param_screen_state_t* param_screen_st = (pal_param_screen_state_t*) param_payload;
            PAL_INFO(LOG_TAG, "Screen State:%d", param_screen_st->screen_state);
            if (payload_size == sizeof(pal_param_screen_state_t)) {
                status = handleScreenStatusChange(*param_screen_st);
            } else {
                PAL_ERR(LOG_TAG,"Incorrect size : expected (%zu), received(%zu)",
                        sizeof(pal_param_screen_state_t), payload_size);
                status = -EINVAL;
            }
        }
        break;
        case PAL_PARAM_ID_DEVICE_ROTATION:
        {
            pal_param_device_rotation_t* param_device_rot =
                                   (pal_param_device_rotation_t*) param_payload;
            PAL_INFO(LOG_TAG, "Device Rotation :%d", param_device_rot->rotation_type);
            if (payload_size == sizeof(pal_param_device_rotation_t)) {
                status = handleDeviceRotationChange(*param_device_rot);
            } else {
                PAL_ERR(LOG_TAG,"Incorrect size : expected (%zu), received(%zu)",
                        sizeof(pal_param_device_rotation_t), payload_size);
                status = -EINVAL;
            }

        }
        break;
        case PAL_PARAM_ID_SP_MODE:
        {
            pal_spkr_prot_payload *spModeval =
                    (pal_spkr_prot_payload *) param_payload;

            if (payload_size == sizeof(pal_spkr_prot_payload)) {
                switch(spModeval->operationMode) {
                    case PAL_SP_MODE_DYNAMIC_CAL:
                    {
                        struct pal_device dattr;
                        dattr.id = PAL_DEVICE_OUT_SPEAKER;
                        std::shared_ptr<Device> dev = nullptr;

                        memset (&mSpkrProtModeValue, 0,
                                        sizeof(pal_spkr_prot_payload));
                        mSpkrProtModeValue.operationMode =
                                PAL_SP_MODE_DYNAMIC_CAL;

                        dev = Device::getInstance(&dattr , rm);
                        if (dev) {
                            PAL_DBG(LOG_TAG, "Got Speaker instance");
                            dev->setParameter(PAL_SP_MODE_DYNAMIC_CAL, nullptr);
                        }
                        else {
                            PAL_DBG(LOG_TAG, "Unable to get speaker instance");
                        }
                    }
                    break;
                    case PAL_SP_MODE_FACTORY_TEST:
                    {
                        memset (&mSpkrProtModeValue, 0,
                                        sizeof(pal_spkr_prot_payload));
                        mSpkrProtModeValue.operationMode =
                                PAL_SP_MODE_FACTORY_TEST;
                        mSpkrProtModeValue.spkrHeatupTime =
                                spModeval->spkrHeatupTime;
                        mSpkrProtModeValue.operationModeRunTime =
                                spModeval->operationModeRunTime;
                    }
                    break;
                    case PAL_SP_MODE_V_VALIDATION:
                    {
                        memset (&mSpkrProtModeValue, 0,
                                        sizeof(pal_spkr_prot_payload));
                        mSpkrProtModeValue.operationMode =
                                PAL_SP_MODE_V_VALIDATION;
                        mSpkrProtModeValue.spkrHeatupTime =
                                spModeval->spkrHeatupTime;
                        mSpkrProtModeValue.operationModeRunTime =
                                spModeval->operationModeRunTime;
                    }
                    break;
                }
            } else {
                PAL_ERR(LOG_TAG,"Incorrect size : expected (%zu), received(%zu)",
                        sizeof(pal_param_device_rotation_t), payload_size);
                status = -EINVAL;
            }
        }
        break;
        case PAL_PARAM_ID_DEVICE_CONNECTION:
        {
            pal_param_device_connection_t *device_connection =
                (pal_param_device_connection_t *)param_payload;
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;

            PAL_INFO(LOG_TAG, "Device %d connected = %d",
                        device_connection->id,
                        device_connection->connection_state);
            if (payload_size == sizeof(pal_param_device_connection_t)) {
                status = handleDeviceConnectionChange(*device_connection);
                if (!status && (device_connection->id == PAL_DEVICE_OUT_BLUETOOTH_A2DP)) {
                    dattr.id = device_connection->id;
                    dev = Device::getInstance(&dattr, rm);
                    status = dev->setDeviceParameter(param_id, param_payload);
                } else {
                    status = SwitchSVADevices(
                        device_connection->connection_state,
                        device_connection->id);
                    if (status) {
                        PAL_ERR(LOG_TAG, "Failed to switch device for SVA");
                    }
                }
            } else {
                PAL_ERR(LOG_TAG,"Incorrect size : expected (%zu), received(%zu)",
                      sizeof(pal_param_device_connection_t), payload_size);
                status = -EINVAL;
            }
        }
        break;
        case PAL_PARAM_ID_CHARGING_STATE:
        {
            pal_param_charging_state *battery_charging_state =
                (pal_param_charging_state *)param_payload;
            StreamSoundTrigger *st_str = nullptr;
            if (IsTransitToNonLPIOnChargingSupported()) {
                if (payload_size == sizeof(pal_param_charging_state)) {
                    PAL_INFO(LOG_TAG, "Charging State = %d",
                              battery_charging_state->charging_state);
                    if (charging_state_ ==
                        battery_charging_state->charging_state) {
                        PAL_DBG(LOG_TAG, "Charging state unchanged, ignore");
                        break;
                    }
                    charging_state_ = battery_charging_state->charging_state;
                    for (int i = 0; i < active_streams_st.size(); i++) {
                        st_str = active_streams_st[i];
                        if (st_str &&
                            isStreamActive(st_str, active_streams_st)) {
                            mResourceManagerMutex.unlock();
                            status = st_str->HandleChargingStateUpdate(
                                battery_charging_state->charging_state, false);
                            mResourceManagerMutex.lock();
                            if (status) {
                                PAL_ERR(LOG_TAG,
                                        "Failed to handling charging state\n");
                            }
                        }
                    }
                    for (int i = 0; i < active_streams_st.size(); i++) {
                        st_str = active_streams_st[i];
                        if (st_str &&
                            isStreamActive(st_str, active_streams_st)) {
                            mResourceManagerMutex.unlock();
                            status = st_str->HandleChargingStateUpdate(
                                battery_charging_state->charging_state, true);
                            mResourceManagerMutex.lock();
                            if (status) {
                                PAL_ERR(LOG_TAG,
                                        "Failed to handling charging state\n");
                            }
                        }
                    }
                } else {
                    PAL_ERR(LOG_TAG,
                            "Incorrect size : expected (%zu), received(%zu)",
                            sizeof(pal_param_charging_state), payload_size);
                    status = -EINVAL;
                }
            } else {
                PAL_DBG(LOG_TAG,
                          "transit_to_non_lpi_on_charging set to false\n");
            }
        }
        break;
        case PAL_PARAM_ID_BT_SCO_WB:
        case PAL_PARAM_ID_BT_SCO_SWB:
        case PAL_PARAM_ID_BT_SCO:
        {
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;

            dattr.id = PAL_DEVICE_OUT_BLUETOOTH_SCO;
            if (isDeviceAvailable(dattr.id)) {
                dev = Device::getInstance(&dattr, rm);
                status = dev->setDeviceParameter(param_id, param_payload);
            }

            dattr.id = PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
            if (isDeviceAvailable(dattr.id)) {
                dev = Device::getInstance(&dattr, rm);
                status = dev->setDeviceParameter(param_id, param_payload);
            }
        }
        break;
        case PAL_PARAM_ID_BT_A2DP_RECONFIG:
        {
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;
            pal_param_bta2dp_t *param_bt_a2dp = nullptr;
            struct pal_device_info devinfo = {};

            dattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            if (isDeviceAvailable(dattr.id)) {
                dev = Device::getInstance(&dattr, rm);
                status = dev->setDeviceParameter(param_id, param_payload);
                if (status) {
                    PAL_ERR(LOG_TAG, "set Parameter %d failed\n", param_id);
                    goto exit;
                }
                status = dev->getDeviceParameter(param_id, (void **)&param_bt_a2dp);
                if (status) {
                    PAL_ERR(LOG_TAG, "get Parameter %d failed\n", param_id);
                    goto exit;
                }
                if (param_bt_a2dp->reconfigured == true) {
                    struct pal_device spkrDattr;
                    std::shared_ptr<Device> spkrDev = nullptr;

                    struct pal_device_info devinfo = {};

                    PAL_DBG(LOG_TAG, "Switching A2DP Device\n");
                    spkrDattr.id = PAL_DEVICE_OUT_SPEAKER;
                    spkrDev = Device::getInstance(&spkrDattr, rm);

                    /* Num channels for Rx devices is same for all usecases,
                       stream type is irrelevant here. */
                    getDeviceInfo(spkrDattr.id, PAL_STREAM_LOW_LATENCY, &devinfo);
                    if ((devinfo.channels == 0) ||
                          (devinfo.channels > devinfo.max_channels)) {
                        status = -EINVAL;
                        PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                        goto exit;
                    }

                    getDeviceConfig(&spkrDattr, NULL, devinfo.channels);
                    getDeviceInfo(dattr.id, PAL_STREAM_LOW_LATENCY, &devinfo);
                    if ((devinfo.channels == 0) ||
                          (devinfo.channels > devinfo.max_channels)) {
                        status = -EINVAL;
                        PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                        goto exit;
                    }
                    getDeviceConfig(&dattr, NULL, devinfo.channels);

                    mResourceManagerMutex.unlock();
                    forceDeviceSwitch(dev, &spkrDattr);
                    forceDeviceSwitch(spkrDev, &dattr);
                    mResourceManagerMutex.lock();
                    param_bt_a2dp->reconfigured = false;
                    dev->setDeviceParameter(param_id, param_bt_a2dp);
                }
            }
        }
        break;
        case PAL_PARAM_ID_BT_A2DP_SUSPENDED:
        {
            std::shared_ptr<Device> a2dp_dev = nullptr;
            struct pal_device a2dp_dattr;
            pal_param_bta2dp_t *current_param_bt_a2dp = nullptr;
            pal_param_bta2dp_t *param_bt_a2dp = nullptr;

            a2dp_dattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            if (!isDeviceAvailable(a2dp_dattr.id)) {
                PAL_ERR(LOG_TAG, "device %d is inactive, set param %d failed\n",
                                  a2dp_dattr.id,  param_id);
                status = -EIO;
                goto exit;
            }

            param_bt_a2dp = (pal_param_bta2dp_t *)param_payload;
            a2dp_dev = Device::getInstance(&a2dp_dattr , rm);
            status = a2dp_dev->getDeviceParameter(param_id, (void **)&current_param_bt_a2dp);
            if (current_param_bt_a2dp->a2dp_suspended == param_bt_a2dp->a2dp_suspended) {
                PAL_INFO(LOG_TAG, "A2DP already in requested state, ignoring\n");
                goto exit;
            }

            if (param_bt_a2dp->a2dp_suspended == false) {
                /* Handle bt sco mic running usecase */
                struct pal_device sco_tx_dattr;
                struct pal_device_info devinfo = {};
                struct pal_stream_attributes sAttr;
                Stream *stream = NULL;
                std::vector<Stream*> activestreams;

                sco_tx_dattr.id = PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
                PAL_DBG(LOG_TAG, "a2dp resumed, switch bt sco mic to handset mic");
                if (isDeviceAvailable(sco_tx_dattr.id)) {
                    struct pal_device handset_tx_dattr;
                    std::shared_ptr<Device> sco_tx_dev = nullptr;

                    handset_tx_dattr.id = PAL_DEVICE_IN_HANDSET_MIC;
                    sco_tx_dev = Device::getInstance(&sco_tx_dattr , rm);
                    getActiveStream_l(sco_tx_dev, activestreams);
                    if (activestreams.size() == 0) {
                       PAL_ERR(LOG_TAG, "no other active streams found");
                       goto setdevparam;
                    }
                    stream = static_cast<Stream *>(activestreams[0]);
                    stream->getStreamAttributes(&sAttr);
                    getDeviceInfo(handset_tx_dattr.id, sAttr.type, &devinfo);
                    PAL_DBG(LOG_TAG, "devinfo.channels %d sAttr.type %d \n", devinfo.channels, sAttr.type);
                    getDeviceConfig(&handset_tx_dattr, &sAttr, devinfo.channels);
                    mResourceManagerMutex.unlock();
                    rm->forceDeviceSwitch(sco_tx_dev, &handset_tx_dattr);
                    mResourceManagerMutex.lock();
                }
                /* TODO : Handle other things in BT class */
            }
setdevparam:
            status = a2dp_dev->setDeviceParameter(param_id, param_payload);
            if (status) {
                PAL_ERR(LOG_TAG, "set Parameter %d failed\n", param_id);
                goto exit;
            }
        }
        break;
        case PAL_PARAM_ID_BT_A2DP_TWS_CONFIG:
        {
            std::shared_ptr<Device> dev = nullptr;
            struct pal_device dattr;

            dattr.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
            if (isDeviceAvailable(dattr.id)) {
                dev = Device::getInstance(&dattr, rm);
                status = dev->setDeviceParameter(param_id, param_payload);
                if (status) {
                    PAL_ERR(LOG_TAG, "set Parameter %d failed\n", param_id);
                    goto exit;
                }
            }
        }
        break;
        case PAL_PARAM_ID_GAIN_LVL_CAL:
        {
            struct pal_device dattr;
            Stream *stream = NULL;
            std::vector<Stream*> activestreams;
            struct pal_stream_attributes sAttr;
            Session *session = NULL;

            pal_param_gain_lvl_cal_t *gain_lvl_cal = (pal_param_gain_lvl_cal_t *) param_payload;
            if (payload_size != sizeof(pal_param_gain_lvl_cal_t)) {
                PAL_ERR(LOG_TAG, "incorrect payload size : expected (%zu), received(%zu)",
                      sizeof(pal_param_gain_lvl_cal_t), payload_size);
                status = -EINVAL;
                goto exit;
            }

            for (int i = 0; i < active_devices.size(); i++) {
                int deviceId = active_devices[i].first->getSndDeviceId();
                status = active_devices[i].first->getDeviceAttributes(&dattr);
                if (0 != status) {
                   PAL_ERR(LOG_TAG,"getDeviceAttributes Failed");
                   goto exit;
                }
                if ((PAL_DEVICE_OUT_SPEAKER == deviceId) ||
                    (PAL_DEVICE_OUT_WIRED_HEADSET == deviceId) ||
                    (PAL_DEVICE_OUT_WIRED_HEADPHONE == deviceId)) {
                    status = getActiveStream_l(active_devices[i].first, activestreams);
                    if ((0 != status) || (activestreams.size() == 0)) {
                       PAL_ERR(LOG_TAG, "no other active streams found");
                       status = -EINVAL;
                       goto exit;
                    }

                    stream = static_cast<Stream *>(activestreams[0]);
                    stream->getStreamAttributes(&sAttr);
                    if ((sAttr.direction == PAL_AUDIO_OUTPUT) &&
                        ((sAttr.type == PAL_STREAM_LOW_LATENCY) ||
                        (sAttr.type == PAL_STREAM_DEEP_BUFFER) ||
                        (sAttr.type == PAL_STREAM_COMPRESSED) ||
                        (sAttr.type == PAL_STREAM_PCM_OFFLOAD))) {
                        stream->setGainLevel(gain_lvl_cal->level);
                        stream->getAssociatedSession(&session);
                        status = session->setConfig(stream, CALIBRATION, TAG_DEVICE_PP_MBDRC);
                        if (0 != status) {
                            PAL_ERR(LOG_TAG, "session setConfig failed with status %d", status);
                            goto exit;
                        }
                    }
                }
            }
        }
        break;
        case PAL_PARAM_ID_UIEFFECT:
        {
#if 0
            gef_payload_t *gef_payload = (gef_payload_t*)param_payload;
            int index = 0;
            int pal_device_id = 0;
            int stream_type = 0;
            bool match = false;
            std::vector<Stream*>::iterator sIter;
            for(sIter = mActiveStreams.begin(); sIter != mActiveStreams.end(); sIter++) {
                match = (*sIter)->isGKVMatch(gef_payload->graph);
                if (match) {
                    pal_param_payload pal_payload;
                    pal_payload.payload = (uint8_t *)&gef_payload->data;
                    status = (*sIter)->setParameters(param_id, (void *)&pal_payload);
                }
            }
#endif
        }
        break;
        default:
            PAL_ERR(LOG_TAG, "Unknown ParamID:%d", param_id);
            break;
    }

exit:
    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit status: %d", status);
    return status;
}


int ResourceManager::setParameter(uint32_t param_id, void *param_payload,
                                  size_t payload_size __unused,
                                  pal_device_id_t pal_device_id,
                                  pal_stream_type_t pal_stream_type)
{
    int status = 0;

    PAL_DBG(LOG_TAG, "Enter param id: %d", param_id);

    mResourceManagerMutex.lock();
    switch (param_id) {
        case PAL_PARAM_ID_UIEFFECT:
        {
            bool match = false;
            std::vector<Stream*>::iterator sIter;
            for(sIter = mActiveStreams.begin(); sIter != mActiveStreams.end();
                    sIter++) {
                match = (*sIter)->checkStreamMatch(pal_device_id,
                                                    pal_stream_type);
                if (match) {
                    status = (*sIter)->setParameters(param_id, param_payload);
                    if (status) {
                        PAL_ERR(LOG_TAG, "failed to set param for pal_device_id=%x stream_type=%x",
                                pal_device_id, pal_stream_type);
                    }
                }
            }
        }
        break;
        default:
            PAL_ERR(LOG_TAG, "Unknown ParamID:%d", param_id);
            break;
    }

    mResourceManagerMutex.unlock();
    PAL_DBG(LOG_TAG, "Exit status: %d",status);
    return status;
}

int ResourceManager::handleScreenStatusChange(pal_param_screen_state_t screen_state)
{
    int status = 0;

    if (screen_state_ != screen_state.screen_state) {
        if (screen_state.screen_state == false) {
            /* have appropriate streams transition to LPI */
            PAL_VERBOSE(LOG_TAG, "Screen State printout");
        }
        else {
            /* have appropriate streams transition out of LPI */
            PAL_VERBOSE(LOG_TAG, "Screen State printout");
        }
        screen_state_ = screen_state.screen_state;
        /* update
         * for (typename std::vector<StreamSoundTrigger*>::iterator iter = active_streams_st.begin();
         *    iter != active_streams_st.end(); iter++) {
         *   status = (*iter)->handleScreenState(screen_state_);
         *  }
         */
    }
    return status;
}

int ResourceManager::handleDeviceRotationChange (pal_param_device_rotation_t
                                                         rotation_type) {
    std::vector<Stream*>::iterator sIter;
    pal_stream_type_t streamType;
    struct pal_device dattr;
    int status = 0;
    PAL_INFO(LOG_TAG, "Device Rotation Changed %d", rotation_type.rotation_type);
    rotation_type_ = rotation_type.rotation_type;

    /**Get the active device list and check if speaker is present.
     */
    for (int i = 0; i < active_devices.size(); i++) {
        int deviceId = active_devices[i].first->getSndDeviceId();
        status = active_devices[i].first->getDeviceAttributes(&dattr);
        if(0 != status) {
           PAL_ERR(LOG_TAG,"getDeviceAttributes Failed");
           goto error;
        }
        PAL_INFO(LOG_TAG, "Device Got %d with channel %d",deviceId,
                                                 dattr.config.ch_info.channels);
        if ((PAL_DEVICE_OUT_SPEAKER == deviceId) &&
            (2 == dattr.config.ch_info.channels)) {

            PAL_INFO(LOG_TAG, "Device is Stereo Speaker");
            std::vector <Stream *> activeStreams;
            getActiveStream_l(active_devices[i].first, activeStreams);
            for (sIter = activeStreams.begin(); sIter != activeStreams.end(); sIter++) {
                status = (*sIter)->getStreamType(&streamType);
                if(0 != status) {
                   PAL_ERR(LOG_TAG,"setParameters Failed");
                   goto error;
                }
                /** Check for the Streams which can require Stereo speaker functionality.
                 * Mainly these will need :
                 * 1. Deep Buffer
                 * 2. PCM offload
                 * 3. Compressed
                 */
                if ((PAL_STREAM_DEEP_BUFFER == streamType) ||
                    (PAL_STREAM_COMPRESSED == streamType) ||
                    (PAL_STREAM_PCM_OFFLOAD == streamType)) {

                    PAL_INFO(LOG_TAG, "Rotation for stream %d", streamType);
                    // Need to set the rotation now.
                    status = (*sIter)->setParameters(PAL_PARAM_ID_DEVICE_ROTATION,
                                                     (void*)&rotation_type);
                    if(0 != status) {
                       PAL_ERR(LOG_TAG,"setParameters Failed");
                       goto error;
                    }
                }
            }
            //As we got the speaker and it is reversed. No need to further
            // iterate the list.
            break;
        }
    }
error :
    PAL_INFO(LOG_TAG, "Exiting handleDeviceRotationChange");
    return status;
}

bool ResourceManager::getScreenState()
{
    return screen_state_;
}

pal_speaker_rotation_type ResourceManager::getCurrentRotationType()
{
    return rotation_type_;
}

int ResourceManager::getDeviceDefaultCapability(pal_param_device_capability_t capability) {
    int status = 0;
    pal_device_id_t device_pal_id = capability.id;
    bool device_available = isDeviceAvailable(device_pal_id);

    struct pal_device conn_device;
    std::shared_ptr<Device> dev = nullptr;
    std::shared_ptr<Device> candidate_device;

    memset(&conn_device, 0, sizeof(struct pal_device));
    conn_device.id = device_pal_id;
    PAL_DBG(LOG_TAG, "device pal id=%x available=%x", device_pal_id, device_available);
    dev = Device::getInstance(&conn_device, rm);
    if (dev)
        status = dev->getDefaultConfig(capability);
    else
        PAL_ERR(LOG_TAG, "failed to get device instance.");

    return status;
}

int ResourceManager::handleDeviceConnectionChange(pal_param_device_connection_t connection_state) {
    int status = 0;
    pal_device_id_t device_id = connection_state.id;
    bool is_connected = connection_state.connection_state;
    bool device_available = isDeviceAvailable(device_id);
    struct pal_device dAttr;
    struct pal_device conn_device;
    std::shared_ptr<Device> dev = nullptr;
    struct pal_device_info devinfo = {};

    PAL_DBG(LOG_TAG, "Enter");
    memset(&conn_device, 0, sizeof(struct pal_device));
    if (is_connected && !device_available) {
        if (isPluginDevice(device_id)) {
            conn_device.id = device_id;
            dev = Device::getInstance(&conn_device, rm);
            if (dev) {
                addPlugInDevice(dev, connection_state);
            } else {
                PAL_ERR(LOG_TAG, "Device creation failed");
                throw std::runtime_error("failed to create device object");
            }
        } else if (isDpDevice(device_id)) {
            conn_device.id = device_id;
            dev = Device::getInstance(&conn_device, rm);
            if (dev) {
                addPlugInDevice(dev, connection_state);
            } else {
                PAL_ERR(LOG_TAG, "Device creation failed");
                throw std::runtime_error("failed to create device object");
            }
        }

        PAL_DBG(LOG_TAG, "Mark device %d as available", device_id);
        if (device_id == PAL_DEVICE_OUT_BLUETOOTH_A2DP) {
            dAttr.id = device_id;
            /* Stream type is irrelevant here as we need device num channels
               which is independent of stype for BT devices */
            rm->getDeviceInfo(dAttr.id, PAL_STREAM_LOW_LATENCY, &devinfo);
            if ((devinfo.channels == 0) ||
                   (devinfo.channels > devinfo.max_channels)) {
                PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                return -EINVAL;
            }
            status = getDeviceConfig(&dAttr, NULL, devinfo.channels);
            if (status) {
                PAL_ERR(LOG_TAG, "Device config not overwritten %d", status);
                return status;
            }
            dev = Device::getInstance(&dAttr, rm);
            if (!dev) {
                PAL_ERR(LOG_TAG, "Device creation failed");
                return -EINVAL;
            }
        } else if (isBtScoDevice(device_id)) {
            dAttr.id = device_id;
            /* Stream type is irrelevant here as we need device num channels
               which is independent of stype for BT devices */
            rm->getDeviceInfo(dAttr.id, PAL_STREAM_LOW_LATENCY, &devinfo);
            if ((devinfo.channels == 0) ||
                   (devinfo.channels > devinfo.max_channels)) {
                PAL_ERR(LOG_TAG, "Invalid num channels [%d], exiting", devinfo.channels);
                return -EINVAL;
            }
            status = getDeviceConfig(&dAttr, NULL, devinfo.channels);
            if (status) {
                PAL_ERR(LOG_TAG, "Device config not overwritten %d", status);
                return status;
            }
            dev = Device::getInstance(&dAttr, rm);
            if (!dev) {
                PAL_ERR(LOG_TAG, "Device creation failed");
                throw std::runtime_error("failed to create device object");
                return -EIO;
            }
        }
        avail_devices_.push_back(device_id);
    } else if (!is_connected && device_available) {
        if (isPluginDevice(device_id)) {
            conn_device.id = device_id;
            removePlugInDevice(device_id, connection_state);
        } else if (isDpDevice(device_id)) {
            conn_device.id = device_id;
            removePlugInDevice(device_id, connection_state);
        }

        PAL_DBG(LOG_TAG, "Mark device %d as unavailable", device_id);
        avail_devices_.erase(std::find(avail_devices_.begin(), avail_devices_.end(), device_id));
    } else {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid operation, connection state %d, device avalibilty %d",
                is_connected, device_available);
    }

    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return status;
}

int ResourceManager::resetStreamInstanceID(Stream *s){
    return s ? resetStreamInstanceID(s, s->getInstanceId()) : -EINVAL;
}

int ResourceManager::resetStreamInstanceID(Stream *str, uint32_t sInstanceID) {
    int status = 0;
    pal_stream_attributes StrAttr;
    KeyVect_t streamConfigModifierKV;

    if(sInstanceID < INSTANCE_1){
        PAL_ERR(LOG_TAG,"Invalid Stream Instance ID\n");
        return -EINVAL;
    }

    status = str->getStreamAttributes(&StrAttr);
    if (status != 0) {
        PAL_ERR(LOG_TAG,"getStreamAttributes Failed \n");
        return status;
    }

    mResourceManagerMutex.lock();

    switch (StrAttr.type) {
        case PAL_STREAM_VOICE_UI:
            streamConfigModifierKV = str->getStreamModifiers();

            if (streamConfigModifierKV.size() == 0) {
                PAL_DBG(LOG_TAG, "no streamConfigModifierKV");
                break;
            }

            for (int x = 0; x < STInstancesLists.size(); x++) {
                if (STInstancesLists[x].first == streamConfigModifierKV[0].second) {
                    PAL_DBG(LOG_TAG,"Found matching StreamConfig(%x) in STInstancesLists(%d)",
                        streamConfigModifierKV[0].second, x);

                    for (int i = 0; i < max_session_num; i++) {
                        if (STInstancesLists[x].second[i].first == sInstanceID){
                            STInstancesLists[x].second[i].second = false;
                            PAL_DBG(LOG_TAG,"ListNodeIndex(%d), InstanceIndex(%d)"
                                  "Instance(%d) to false",
                                  x,
                                  i,
                                  sInstanceID);
                            break;
                        }
                    }
                    break;
                }
            }
            break;
        case PAL_STREAM_NON_TUNNEL:
            if (stream_instances[StrAttr.type - 1] > 0)
                stream_instances[StrAttr.type - 1] -= 1;
            str->setInstanceId(0);
            break;
        default:
            stream_instances[StrAttr.type - 1] &= ~(1 << (sInstanceID - 1));
            str->setInstanceId(0);
    }

    mResourceManagerMutex.unlock();
    return status;
}

int ResourceManager::getStreamInstanceID(Stream *str) {
    int i, status = 0, listNodeIndex = -1;
    pal_stream_attributes StrAttr;
    KeyVect_t streamConfigModifierKV;

    status = str->getStreamAttributes(&StrAttr);

    if (status != 0) {
        PAL_ERR(LOG_TAG,"getStreamAttributes Failed \n");
        return status;
    }

    mResourceManagerMutex.lock();

    switch (StrAttr.type) {
        case PAL_STREAM_VOICE_UI:
            PAL_DBG(LOG_TAG,"STInstancesLists.size (%zu)", STInstancesLists.size());

            streamConfigModifierKV = str->getStreamModifiers();

            if (streamConfigModifierKV.size() == 0) {
                PAL_DBG(LOG_TAG, "no streamConfigModifierKV");
                break;
            }

            for (int x = 0; x < STInstancesLists.size(); x++) {
                if (STInstancesLists[x].first == streamConfigModifierKV[0].second) {
                    PAL_DBG(LOG_TAG,"Found list for StreamConfig(%x),index(%d)",
                        streamConfigModifierKV[0].second, x);
                    listNodeIndex = x;
                    break;
                }
            }

            if (listNodeIndex < 0) {
                InstanceListNode_t streamConfigInstanceList;
                PAL_DBG(LOG_TAG,"Create InstanceID list for streamConfig(%x)",
                    streamConfigModifierKV[0].second);

                STInstancesLists.push_back(make_pair(
                    streamConfigModifierKV[0].second,
                    streamConfigInstanceList));
                //Initialize List
                for (i = 1; i <= max_session_num; i++) {
                    STInstancesLists.back().second.push_back(std::make_pair(i, false));
                }
                listNodeIndex = STInstancesLists.size() - 1;
            }

            for (i = 0; i < max_session_num; i++) {
                if (!STInstancesLists[listNodeIndex].second[i].second) {
                    STInstancesLists[listNodeIndex].second[i].second = true;
                    status = STInstancesLists[listNodeIndex].second[i].first;
                    PAL_DBG(LOG_TAG,"ListNodeIndex(%d), InstanceIndex(%d)"
                          "Instance(%d) to true",
                          listNodeIndex,
                          i,
                          status);
                    break;
                }
            }
            break;
        case PAL_STREAM_NON_TUNNEL:
            status = str->getInstanceId();
            if (!status) {
                if (stream_instances[StrAttr.type - 1] == max_nt_sessions) {
                    PAL_ERR(LOG_TAG, "All stream instances taken");
                    status = -EINVAL;
                    break;
                }
                status = stream_instances[StrAttr.type - 1] + 1;
                stream_instances[StrAttr.type - 1] = stream_instances[StrAttr.type - 1] + 1;
                str->setInstanceId(status);
            }
            break;
        default:
            status = str->getInstanceId();
            if (!status) {
                if (stream_instances[StrAttr.type - 1] ==  -1) {
                    PAL_ERR(LOG_TAG, "All stream instances taken");
                    status = -EINVAL;
                    break;
                }
                for (i = 0; i < MAX_STREAM_INSTANCES; ++i)
                    if (!(stream_instances[StrAttr.type - 1] & (1 << i))) {
                        stream_instances[StrAttr.type - 1] |= (1 << i);
                        status = i + 1;
                        break;
                    }
                str->setInstanceId(status);
            }
    }

    mResourceManagerMutex.unlock();
    return status;
}

bool ResourceManager::isDeviceAvailable(pal_device_id_t id)
{
    bool is_available = false;
    typename std::vector<pal_device_id_t>::iterator iter =
        std::find(avail_devices_.begin(), avail_devices_.end(), id);

    if (iter != avail_devices_.end())
        is_available = true;

    PAL_DBG(LOG_TAG, "Device %d, is_available = %d", id, is_available);

    return is_available;
}

bool ResourceManager::isDeviceReady(pal_device_id_t id)
{
    struct pal_device dAttr;
    std::shared_ptr<Device> dev = nullptr;
    bool is_ready = false;

    switch (id) {
        case PAL_DEVICE_OUT_BLUETOOTH_SCO:
        case PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET:
        case PAL_DEVICE_OUT_BLUETOOTH_A2DP:
        case PAL_DEVICE_IN_BLUETOOTH_A2DP:
        {
            if (!isDeviceAvailable(id))
                return is_ready;

            dAttr.id = id;
            dev = Device::getInstance((struct pal_device *)&dAttr , rm);
            if (!dev) {
                PAL_ERR(LOG_TAG, "Device getInstance failed");
                return false;
            }
            is_ready = dev->isDeviceReady();
            break;
        }
        default:
            is_ready = true;
            break;
    }

    return is_ready;
}

bool ResourceManager::isBtScoDevice(pal_device_id_t id)
{
    if (id == PAL_DEVICE_OUT_BLUETOOTH_SCO ||
        id == PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET)
        return true;
    else
        return false;
}

void ResourceManager::updateBtCodecMap(std::pair<uint32_t, std::string> key, std::string value)
{
    btCodecMap.insert(std::make_pair(key, value));
}

std::string ResourceManager::getBtCodecLib(uint32_t codecFormat, std::string codecType)
{
    std::map<std::pair<uint32_t, std::string>, std::string>::iterator iter;

    iter = btCodecMap.find(std::make_pair(codecFormat, codecType));
    if (iter != btCodecMap.end()) {
        return iter->second;
    }

    return std::string();
}

void ResourceManager::processBTCodecInfo(const XML_Char **attr)
{
    char *saveptr = NULL;
    char *token = NULL;
    std::vector<std::string> codec_formats, codec_types;
    std::vector<std::string>::iterator iter1, iter2;
    std::map<std::string, uint32_t>::iterator iter;

    if (strcmp(attr[0], "codec_format") != 0) {
        PAL_ERR(LOG_TAG,"'codec_format' not found");
        goto done;
    }

    if (strcmp(attr[2], "codec_type") != 0) {
        PAL_ERR(LOG_TAG,"'codec_type' not found");
        goto done;
    }

    if (strcmp(attr[4], "codec_library") != 0) {
        PAL_ERR(LOG_TAG,"'codec_library' not found");
        goto done;
    }

    token = strtok_r((char *)attr[1], "|", &saveptr);
    while (token != NULL) {
        if (strlen(token) != 0) {
            codec_formats.push_back(std::string(token));
        }
        token = strtok_r(NULL, "|", &saveptr);
    }

    token = strtok_r((char *)attr[3], "|", &saveptr);
    while (token != NULL) {
        if (strlen(token) != 0) {
            codec_types.push_back(std::string(token));
        }
        token = strtok_r(NULL, "|", &saveptr);
    }

    for (iter1 = codec_formats.begin(); iter1 != codec_formats.end(); ++iter1) {
        for (iter2 = codec_types.begin(); iter2 != codec_types.end(); ++iter2) {
            PAL_VERBOSE(LOG_TAG, "BT Codec Info %s=%s, %s=%s, %s=%s",
                    attr[0], (*iter1).c_str(), attr[2], (*iter2).c_str(), attr[4], attr[5]);

            iter = btFmtTable.find(*iter1);
            if (iter != btFmtTable.end()) {
                updateBtCodecMap(std::make_pair(btFmtTable[*iter1], *iter2),  std::string(attr[5]));
            }
        }
    }

done:
    return;
}

bool ResourceManager::isPluginDevice(pal_device_id_t id) {
    if (id == PAL_DEVICE_OUT_USB_DEVICE ||
        id == PAL_DEVICE_OUT_USB_HEADSET ||
        id == PAL_DEVICE_IN_USB_DEVICE ||
        id == PAL_DEVICE_IN_USB_HEADSET)
        return true;
    else
        return false;
}

bool ResourceManager::isDpDevice(pal_device_id_t id) {
    if (id == PAL_DEVICE_OUT_AUX_DIGITAL || id == PAL_DEVICE_OUT_AUX_DIGITAL_1 ||
        id == PAL_DEVICE_OUT_HDMI)
        return true;
    else
        return false;
}

void ResourceManager::processConfigParams(const XML_Char **attr)
{
    if (strcmp(attr[0], "key") != 0) {
        PAL_ERR(LOG_TAG,"'key' not found");
        goto done;
    }

    if (strcmp(attr[2], "value") != 0) {
        PAL_ERR(LOG_TAG,"'value' not found");
        goto done;
    }
    PAL_VERBOSE(LOG_TAG, "String %s %s %s %s ",attr[0],attr[1],attr[2],attr[3]);
    configParamKVPairs = str_parms_create();
    str_parms_add_str(configParamKVPairs, (char*)attr[1], (char*)attr[3]);
    setConfigParams(configParamKVPairs);
    str_parms_destroy(configParamKVPairs);
done:
    return;
}

void ResourceManager::processCardInfo(struct xml_userdata *data, const XML_Char *tag_name)
{
    int card;
    if (!strcmp(tag_name, "id")) {
        card = atoi(data->data_buf);
        data->card_found = true;
    }
}

void ResourceManager::processDeviceIdProp(struct xml_userdata *data, const XML_Char *tag_name)
{
    int device, size = -1;
    struct deviceCap dev;

    memset(&dev, 0, sizeof(struct deviceCap));
    if (!strcmp(tag_name, "pcm-device") ||
        !strcmp(tag_name, "compress-device") ||
        !strcmp(tag_name, "mixer"))
        return;

    if (!strcmp(tag_name, "id")) {
        device = atoi(data->data_buf);
        dev.deviceId = device;
        devInfo.push_back(dev);
    } else if (!strcmp(tag_name, "name")) {
        size = devInfo.size() - 1;
        strlcpy(devInfo[size].name, data->data_buf, strlen(data->data_buf)+1);
        if(strstr(data->data_buf,"PCM")) {
            devInfo[size].type = PCM;
        } else if (strstr(data->data_buf,"COMP")) {
            devInfo[size].type = COMPRESS;
        } else if (strstr(data->data_buf,"VOICEMMODE1")){
            devInfo[size].type = VOICE1;
        } else if (strstr(data->data_buf,"VOICEMMODE2")){
            devInfo[size].type = VOICE2;
        }
    }
}

void ResourceManager::processDeviceCapability(struct xml_userdata *data, const XML_Char *tag_name)
{
    int size = -1;
    int val = -1;
    if (!strlen(data->data_buf) || !strlen(tag_name))
        return;
    if (strcmp(tag_name, "props") == 0)
        return;
    size = devInfo.size() - 1;
    if (strcmp(tag_name, "playback") == 0) {
        val = atoi(data->data_buf);
        devInfo[size].playback = val;
    } else if (strcmp(tag_name, "capture") == 0) {
        val = atoi(data->data_buf);
        devInfo[size].record = val;
    } else if (strcmp(tag_name,"session_mode") == 0) {
        val = atoi(data->data_buf);
        devInfo[size].sess_mode = (sess_mode_t) val;
    }
}

void ResourceManager::process_gain_db_to_level_map(struct xml_userdata *data, const XML_Char **attr)
{
    struct pal_amp_db_and_gain_table tbl_entry;

    if (data->gain_lvl_parsed)
        return;

    if ((strcmp(attr[0], "db") != 0) ||
        (strcmp(attr[2], "level") != 0)) {
        PAL_ERR(LOG_TAG, "invalid attribute passed  %s %sexpected amp db level", attr[0], attr[2]);
        goto done;
    }

    tbl_entry.db = atof(attr[1]);
    tbl_entry.amp = exp(tbl_entry.db * 0.115129f);
    tbl_entry.level = atoi(attr[3]);

    // custome level should be > 0. Level 0 is fixed for default
    if (tbl_entry.level <= 0) {
        PAL_ERR(LOG_TAG, "amp [%f]  db [%f] level [%d]",
               tbl_entry.amp, tbl_entry.db, tbl_entry.level);
        goto done;
    }

    PAL_VERBOSE(LOG_TAG, "amp [%f]  db [%f] level [%d]",
           tbl_entry.amp, tbl_entry.db, tbl_entry.level);

    if (!gainLvlMap.empty() && (gainLvlMap.back().amp >= tbl_entry.amp)) {
        PAL_ERR(LOG_TAG, "value not in ascending order .. rejecting custom mapping");
        gainLvlMap.clear();
        data->gain_lvl_parsed = true;
    }

    gainLvlMap.push_back(tbl_entry);

done:
    return;
}

int ResourceManager::getGainLevelMapping(struct pal_amp_db_and_gain_table *mapTbl, int tblSize)
{
    int size = 0;

    if (gainLvlMap.empty()) {
        PAL_DBG(LOG_TAG, "empty or currupted gain_mapping_table");
        return 0;
    }

    for (; size < gainLvlMap.size() && size <= tblSize; size++) {
        mapTbl[size] = gainLvlMap.at(size);
        PAL_VERBOSE(LOG_TAG, "added amp[%f] db[%f] level[%d]",
                mapTbl[size].amp, mapTbl[size].db, mapTbl[size].level);
    }

    return size;
}

void ResourceManager::snd_reset_data_buf(struct xml_userdata *data)
{
    data->offs = 0;
    data->data_buf[data->offs] = '\0';
}

void ResourceManager::process_voicemode_info(const XML_Char **attr)
{
    std::string tagkey(attr[1]);
    std::string tagvalue(attr[3]);
    struct vsid_modepair modepair = {};

    if (strcmp(attr[0], "key") !=0) {
        PAL_ERR(LOG_TAG, "key not found");
        return;
    }
    modepair.key = convertCharToHex(tagkey);

    if (strcmp(attr[2], "value") !=0) {
        PAL_ERR(LOG_TAG, "value not found");
        return;
    }
    modepair.value = convertCharToHex(tagvalue);
    PAL_INFO(LOG_TAG, "key  %x value  %x", modepair.key, modepair.value);
    vsidInfo.modepair.push_back(modepair);
}

void ResourceManager::process_config_voice(struct xml_userdata *data, const XML_Char *tag_name)
{
    if(data->voice_info_parsed)
        return;

    if (data->offs <= 0)
        return;
    data->data_buf[data->offs] = '\0';
    if (data->tag == TAG_CONFIG_VOICE) {
        if (strcmp(tag_name, "vsid") == 0) {
            std::string vsidvalue(data->data_buf);
            vsidInfo.vsid = convertCharToHex(vsidvalue);
        }
    }
    if (!strcmp(tag_name, "modepair")) {
        data->tag = TAG_CONFIG_MODE_MAP;
    } else if (!strcmp(tag_name, "mode_map")) {
        data->tag = TAG_CONFIG_VOICE;
    } else if (!strcmp(tag_name, "config_voice")) {
        data->tag = TAG_RESOURCE_MANAGER_INFO;
        data->voice_info_parsed = true;
    }
}

void ResourceManager::process_kvinfo(const XML_Char **attr)
{
    struct kvpair_info kv;
    int size = 0, sizeusecase = 0;
    std::string tagkey(attr[1]);
    std::string tagvalue(attr[3]);

    if (strcmp(attr[0], "key") !=0) {
        PAL_ERR(LOG_TAG, "key not found");
        return;
    }
    kv.key = convertCharToHex(tagkey);
    if (strcmp(attr[2], "value") !=0) {
        PAL_ERR(LOG_TAG, "value not found");
        return;
    }
    kv.value = convertCharToHex(tagvalue);

    size = deviceInfo.size() - 1;
    sizeusecase = deviceInfo[size].usecase.size() - 1;
    deviceInfo[size].usecase[sizeusecase].kvpair.push_back(kv);
    PAL_DBG(LOG_TAG, "key  %x value  %x", kv.key, kv.value);
}

void ResourceManager::process_device_info(struct xml_userdata *data, const XML_Char *tag_name)
{

    struct deviceIn dev = {};
    struct usecase_info usecase_data = {};
    int size = -1 , sizeusecase = -1;

    if (data->offs <= 0)
        return;
    data->data_buf[data->offs] = '\0';

    if (data->resourcexml_parsed)
      return;

    if ((data->tag == TAG_IN_DEVICE) || (data->tag == TAG_OUT_DEVICE)) {
        if (!strcmp(tag_name, "id")) {
            std::string deviceName(data->data_buf);
            dev.deviceId  = deviceIdLUT.at(deviceName);
            deviceInfo.push_back(dev);
        } else if (!strcmp(tag_name, "back_end_name")) {
            std::string backendname(data->data_buf);
            size = deviceInfo.size() - 1;
            updateBackEndName(deviceInfo[size].deviceId, backendname);
        } else if (!strcmp(tag_name, "max_channels")) {
            size = deviceInfo.size() - 1;
            deviceInfo[size].max_channel = atoi(data->data_buf);
        } else if (!strcmp(tag_name, "channels")) {
            size = deviceInfo.size() - 1;
            deviceInfo[size].channel = atoi(data->data_buf);
        } else if (!strcmp(tag_name, "snd_device_name")) {
            size = deviceInfo.size() - 1;
            std::string snddevname(data->data_buf);
            updateSndName(deviceInfo[size].deviceId, snddevname);
        } else if (!strcmp(tag_name, "speaker_protection_enabled")) {
            if (atoi(data->data_buf))
                isSpeakerProtectionEnabled = true;
        } else if (!strcmp(tag_name, "cps_enabled")) {
            if (atoi(data->data_buf))
                isCpsEnabled = true;
        } else if (!strcmp(tag_name, "is_24_bit_supported")) {
            if (atoi(data->data_buf))
                bitWidthSupported = BITWIDTH_24;
        } else if (!strcmp(tag_name, "speaker_mono_right")) {
            if (atoi(data->data_buf))
                isMainSpeakerRight = true;
        } else if (!strcmp(tag_name, "quick_cal_time")) {
            spQuickCalTime = atoi(data->data_buf);
        }else if (!strcmp(tag_name, "ras_enabled")) {
            if (atoi(data->data_buf))
                isRasEnabled = true;
        }
    } else if (data->tag == TAG_USECASE) {
        if (!strcmp(tag_name, "name")) {
            std::string userIdname(data->data_buf);
            usecase_data.type  = usecaseIdLUT.at(userIdname);
            size = deviceInfo.size() - 1;
            deviceInfo[size].usecase.push_back(usecase_data);
        } else if (!strcmp(tag_name, "sidetone_mode")) {
            std::string mode(data->data_buf);
            size = deviceInfo.size() - 1;
            sizeusecase = deviceInfo[size].usecase.size() - 1;
            deviceInfo[size].usecase[sizeusecase].sidetoneMode = sidetoneModetoId.at(mode);
        }
    } else if (data->tag == TAG_ECREF) {
        if (!strcmp(tag_name, "id")) {
            std::string rxDeviceName(data->data_buf);
            pal_device_id_t rxDeviceId  = deviceIdLUT.at(rxDeviceName);
            size = deviceInfo.size() - 1;
            deviceInfo[size].rx_dev_ids.push_back(rxDeviceId);
        }
    }
    if (!strcmp(tag_name, "kvpair")) {
        data->tag = TAG_DEVICEPP;
    } else if (!strcmp(tag_name, "devicePP-metadata")) {
        data->tag = TAG_USECASE;
    } else if (!strcmp(tag_name, "usecase")) {
        data->tag = TAG_IN_DEVICE;
    } else if (!strcmp(tag_name, "in-device") || !strcmp(tag_name, "out-device")) {
        data->tag = TAG_DEVICE_PROFILE;
    } else if (!strcmp(tag_name, "device_profile")) {
        data->tag = TAG_RESOURCE_MANAGER_INFO;
    } else if (!strcmp(tag_name, "sidetone_mode")) {
        data->tag = TAG_USECASE;
    } else if (!strcmp(tag_name, "ec_rx_device")) {
        data->tag = TAG_ECREF;
    } else if (!strcmp(tag_name, "resource_manager_info")) {
        data->tag = TAG_RESOURCE_ROOT;
        data->resourcexml_parsed = true;
    }
}

void ResourceManager::process_input_streams(struct xml_userdata *data, const XML_Char *tag_name)
{
    struct tx_ecinfo txecinfo = {};
    int type = 0;
    int size = -1;

    if (data->offs <= 0)
        return;
    data->data_buf[data->offs] = '\0';

    if (data->resourcexml_parsed)
      return;

    if (data->tag == TAG_INSTREAM) {
        if (!strcmp(tag_name, "name")) {
            std::string userIdname(data->data_buf);
            txecinfo.tx_stream_type  = usecaseIdLUT.at(userIdname);
            txEcInfo.push_back(txecinfo);
            PAL_DBG(LOG_TAG, "name %d", txecinfo.tx_stream_type);
        }
    } else if (data->tag == TAG_ECREF) {
        if (!strcmp(tag_name, "disabled_stream")) {
            std::string userIdname(data->data_buf);
            type  = usecaseIdLUT.at(userIdname);
            size = txEcInfo.size() - 1;
            txEcInfo[size].disabled_rx_streams.push_back(type);
            PAL_DBG(LOG_TAG, "ecref %d", type);
        }
    }
    if (!strcmp(tag_name, "in_streams")) {
        data->tag = TAG_INSTREAMS;
    } else if (!strcmp(tag_name, "in_stream")) {
        data->tag = TAG_INSTREAM;
    } else if (!strcmp(tag_name, "policies")) {
        data->tag = TAG_POLICIES;
    } else if (!strcmp(tag_name, "ec_ref")) {
        data->tag = TAG_ECREF;
    } else if (!strcmp(tag_name, "resource_manager_info")) {
        data->tag = TAG_RESOURCE_ROOT;
        data->resourcexml_parsed = true;
    }
}

void ResourceManager::snd_process_data_buf(struct xml_userdata *data, const XML_Char *tag_name)
{
    if (data->offs <= 0)
        return;

    data->data_buf[data->offs] = '\0';

    if (data->card_parsed)
        return;

    if (data->current_tag == TAG_ROOT)
        return;

    if (data->current_tag == TAG_CARD) {
        processCardInfo(data, tag_name);
    } else if (data->current_tag == TAG_PLUGIN) {
        //snd_parse_plugin_properties(data, tag_name);
    } else if (data->current_tag == TAG_DEVICE) {
        //PAL_ERR(LOG_TAG,"tag %s", (char*)tag_name);
        processDeviceIdProp(data, tag_name);
    } else if (data->current_tag == TAG_DEV_PROPS) {
        processDeviceCapability(data, tag_name);
    }
}

void ResourceManager::setGaplessMode(const XML_Char **attr)
{
    if (strcmp(attr[0], "key") != 0) {
        PAL_ERR(LOG_TAG, "key not found");
        return;
    }
    if (strcmp(attr[2], "value") != 0) {
        PAL_ERR(LOG_TAG, "value not found");
        return;
    }
    if (atoi(attr[3])) {
       isGaplessEnabled = true;
       return;
    }
}

void ResourceManager::startTag(void *userdata, const XML_Char *tag_name,
    const XML_Char **attr)
{
    stream_supported_type type;
    struct xml_userdata *data = (struct xml_userdata *)userdata;
    static std::shared_ptr<SoundTriggerPlatformInfo> st_info = nullptr;

    if (data->is_parsing_sound_trigger) {
        st_info->HandleStartTag((const char *)tag_name, (const char **)attr);
        return;
    }

    if (!strcmp(tag_name, "sound_trigger_platform_info")) {
        data->is_parsing_sound_trigger = true;
        st_info = SoundTriggerPlatformInfo::GetInstance();
        return;
    }

    if (strcmp(tag_name, "device") == 0) {
        return;
    } else if(strcmp(tag_name, "param") == 0) {
        processConfigParams(attr);
    } else if (strcmp(tag_name, "codec") == 0) {
        processBTCodecInfo(attr);
        return;
    } else if (strcmp(tag_name, "config_gapless") == 0) {
        setGaplessMode(attr);
        return;
    }

    if (data->card_parsed)
        return;

    snd_reset_data_buf(data);

    if (!strcmp(tag_name, "resource_manager_info")) {
        data->tag = TAG_RESOURCE_MANAGER_INFO;
    } else if (!strcmp(tag_name, "config_voice")) {
        data->tag = TAG_CONFIG_VOICE;
    } else if (!strcmp(tag_name, "mode_map")) {
        data->tag = TAG_CONFIG_MODE_MAP;
    } else if (!strcmp(tag_name, "modepair")) {
        data->tag = TAG_CONFIG_MODE_PAIR;
        process_voicemode_info(attr);
    } else if (!strcmp(tag_name, "gain_db_to_level_mapping")) {
        data->tag = TAG_GAIN_LEVEL_MAP;
    } else if (!strcmp(tag_name, "gain_level_map")) {
        data->tag = TAG_GAIN_LEVEL_PAIR;
        process_gain_db_to_level_map(data, attr);
    } else if (!strcmp(tag_name, "device_profile")) {
        data->tag = TAG_DEVICE_PROFILE;
    } else if (!strcmp(tag_name, "in-device")) {
        data->tag = TAG_IN_DEVICE;
    } else if (!strcmp(tag_name, "out-device")) {
        data->tag = TAG_OUT_DEVICE;
    } else if (!strcmp(tag_name, "usecase")) {
        data->tag = TAG_USECASE;
    } else if (!strcmp(tag_name, "devicePP-metadata")) {
        data->tag = TAG_DEVICEPP;
    } else if (!strcmp(tag_name, "kvpair")) {
        process_kvinfo(attr);
        data->tag = TAG_KVPAIR;
    } else if (!strcmp(tag_name, "in_streams")) {
        data->tag = TAG_INSTREAMS;
    } else if (!strcmp(tag_name, "in_stream")) {
        data->tag = TAG_INSTREAM;
    } else if (!strcmp(tag_name, "policies")) {
        data->tag = TAG_POLICIES;
    } else if (!strcmp(tag_name, "ec_ref")) {
        data->tag = TAG_ECREF;
    } else if (!strcmp(tag_name, "ec_rx_device")) {
        data->tag = TAG_ECREF;
    }else if (!strcmp(tag_name, "sidetone_mode")) {
        data->tag = TAG_USECASE;
    }

    if (!strcmp(tag_name, "card"))
        data->current_tag = TAG_CARD;
    if (strcmp(tag_name, "pcm-device") == 0) {
        type = PCM;
        data->current_tag = TAG_DEVICE;
    } else if (strcmp(tag_name, "compress-device") == 0) {
        data->current_tag = TAG_DEVICE;
        type = COMPRESS;
    } else if (strcmp(tag_name, "mixer") == 0) {
        data->current_tag = TAG_MIXER;
    } else if (strstr(tag_name, "plugin")) {
        data->current_tag = TAG_PLUGIN;
    } else if (!strcmp(tag_name, "props")) {
        data->current_tag = TAG_DEV_PROPS;
    }
    if (data->current_tag != TAG_CARD && !data->card_found)
        return;
}

void ResourceManager::endTag(void *userdata, const XML_Char *tag_name)
{
    struct xml_userdata *data = (struct xml_userdata *)userdata;
    std::shared_ptr<SoundTriggerPlatformInfo> st_info =
        SoundTriggerPlatformInfo::GetInstance();

    if (!strcmp(tag_name, "sound_trigger_platform_info")) {
        data->is_parsing_sound_trigger = false;
        return;
    }

    if (data->is_parsing_sound_trigger) {
        st_info->HandleEndTag((const char *)tag_name);
        return;
    }

    process_config_voice(data,tag_name);
    process_device_info(data,tag_name);
    process_input_streams(data,tag_name);

    if (data->card_parsed)
        return;
    if (data->current_tag != TAG_CARD && !data->card_found)
        return;
    snd_process_data_buf(data, tag_name);
    snd_reset_data_buf(data);
    if (!strcmp(tag_name, "mixer") || !strcmp(tag_name, "pcm-device") || !strcmp(tag_name, "compress-device"))
        data->current_tag = TAG_CARD;
    else if (strstr(tag_name, "plugin") || !strcmp(tag_name, "props"))
        data->current_tag = TAG_DEVICE;
    else if(!strcmp(tag_name, "card")) {
        data->current_tag = TAG_ROOT;
        if (data->card_found)
            data->card_parsed = true;
    }
}

void ResourceManager::snd_data_handler(void *userdata, const XML_Char *s, int len)
{
   struct xml_userdata *data = (struct xml_userdata *)userdata;

    if (data->is_parsing_sound_trigger) {
        SoundTriggerPlatformInfo::GetInstance()->HandleCharData(
            (const char *)s);
        return;
    }

   if (len + data->offs >= sizeof(data->data_buf) ) {
       data->offs += len;
       /* string length overflow, return */
       return;
   } else {
       memcpy(data->data_buf + data->offs, s, len);
       data->offs += len;
   }
}

int ResourceManager::XmlParser(std::string xmlFile)
{
    XML_Parser parser;
    FILE *file = NULL;
    int ret = 0;
    int bytes_read;
    void *buf = NULL;
    struct xml_userdata card_data;
    memset(&card_data, 0, sizeof(card_data));

    PAL_INFO(LOG_TAG, "XML parsing started - file name %s", xmlFile.c_str());
    file = fopen(xmlFile.c_str(), "r");
    if(!file) {
        ret = EINVAL;
        PAL_ERR(LOG_TAG, "Failed to open xml file name %s ret %d", xmlFile.c_str(), ret);
        goto done;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ret = -EINVAL;
        PAL_ERR(LOG_TAG, "Failed to create XML ret %d", ret);
        goto closeFile;
    }
    XML_SetUserData(parser, &card_data);
    XML_SetElementHandler(parser, startTag, endTag);
    XML_SetCharacterDataHandler(parser, snd_data_handler);

    while (1) {
        buf = XML_GetBuffer(parser, 1024);
        if(buf == NULL) {
            ret = -EINVAL;
            PAL_ERR(LOG_TAG, "XML_Getbuffer failed ret %d", ret);
            goto freeParser;
        }

        bytes_read = fread(buf, 1, 1024, file);
        if(bytes_read < 0) {
            ret = -EINVAL;
            PAL_ERR(LOG_TAG, "fread failed ret %d", ret);
            goto freeParser;
        }

        if(XML_ParseBuffer(parser, bytes_read, bytes_read == 0) == XML_STATUS_ERROR) {
            ret = -EINVAL;
            PAL_ERR(LOG_TAG, "XML ParseBuffer failed for %s file ret %d", xmlFile.c_str(), ret);
            goto freeParser;
        }
        if (bytes_read == 0)
            break;
    }

freeParser:
    XML_ParserFree(parser);
closeFile:
    fclose(file);
done:
    return ret;
}
