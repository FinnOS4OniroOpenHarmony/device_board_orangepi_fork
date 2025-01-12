/*
 * Copyright (c) 2022-2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "alsa_lib_common.h"
#include <ctype.h>
#include <limits.h>
#include "audio_common.h"
#include "audio_internal.h"
#include "cJSON.h"
#include "osal_mem.h"
#include "securec.h"

#define HDF_LOG_TAG HDF_AUDIO_HAL_LIB

#define USB_AUDIO             "USB Audio"
#define MIXER_CTL_VOLUME      "simple_mixer_ctl_volume"
#define PLAYBACK              "playback"
#define CAPTURE               "capture"
#define MAX_ELEMENT           100
#define ALSA_CARD_CONFIG_FILE HDF_CONFIG_DIR "/alsa_adapter.json"
#define ALSA_CONFIG_FILE_MAX  (2 * 1024) // 2KB

#define SUPPORT_CAPTURE_OR_RENDER  1
#define SUPPORT_CAPTURE_AND_RENDER 2

struct CardStream {
    int card;
    snd_pcm_stream_t stream; /** Playback stream or Capture stream */
};

static struct AudioCardInfo *g_audioCardIns = NULL;
struct AlsaDevInfo *g_alsadevInfo = NULL;
static bool g_parseFlag = false;

/* Record the list of sound cards read from the alsa device */
static struct AlsaCardsList g_alsaCardsList;
/* Record the number of sound cards configured in the sound card configuration file,
 * which is different from the sound cards read from the alsa device.
 */
static uint8_t g_sndCardsNum = 0;

char *g_usbSimpleRenderCtlVol[] = {
    "Earpiece",
    "Speaker",
    "Headphone",
    "PCM",
    "Mic",
};

char *g_usbSimpleCapCtlVol[] = {
    "Earpiece",
    "Headset",
    "Headphone",
    "Mic",
};

static struct DevProcInfo *g_sndCardList[SND_CARD_MAX][AUDIO_MAX_CARD_NUM] = {{NULL}};

static struct DevProcInfo **AudioGetSoundCardsInfo(enum SndCardType cardType)
{
    return (struct DevProcInfo **)(g_sndCardList + cardType);
}

int32_t InitCardIns(void)
{
    if (g_audioCardIns == NULL) {
        g_audioCardIns = (struct AudioCardInfo *)OsalMemCalloc(MAX_CARD_NUM * sizeof(struct AudioCardInfo));
        if (g_audioCardIns == NULL) {
            AUDIO_FUNC_LOGE("Failed to allocate memory!");
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static struct AudioCardInfo *AddCardIns(const char *cardName)
{
    int32_t i;
    int32_t ret;

    if (cardName == NULL || strlen(cardName) == 0) {
        AUDIO_FUNC_LOGE("Invalid cardName!");
        return NULL;
    }

    if (g_audioCardIns == NULL) {
        AUDIO_FUNC_LOGE("g_audioCardIns is NULL!");
        return NULL;
    }

    for (i = 0; i < MAX_CARD_NUM; i++) {
        if (g_audioCardIns[i].cardStatus == 0) {
            (void)memset_s(&g_audioCardIns[i], sizeof(struct AudioCardInfo), 0, sizeof(struct AudioCardInfo));
            ret = strncpy_s(g_audioCardIns[i].cardName, MAX_CARD_NAME_LEN + 1, cardName, strlen(cardName));
            if (ret != 0) {
                AUDIO_FUNC_LOGE("strncpy_s failed!");
                return NULL;
            }
            g_audioCardIns[i].cardStatus++;
            return &(g_audioCardIns[i]);
        }
    }
    AUDIO_FUNC_LOGE("Failed to AddCardIns!");

    return NULL;
}

static struct AudioCardInfo *FindCardIns(const char *cardName)
{
    int32_t i;

    if (cardName == NULL || strlen(cardName) == 0) {
        AUDIO_FUNC_LOGE("Invalid cardName!");
        return NULL;
    }

    if (g_audioCardIns == NULL) {
        AUDIO_FUNC_LOGE("g_audioCardIns is NULL!");
        return NULL;
    }

    for (i = 0; i < MAX_CARD_NUM; i++) {
        if (strcmp(g_audioCardIns[i].cardName, cardName) == 0) {
            return &(g_audioCardIns[i]);
        }
    }

    return NULL;
}

static int32_t AudioAddCardIns(const char *cardName)
{
    struct AudioCardInfo *cardInfo = NULL;

    if (cardName == NULL) {
        AUDIO_FUNC_LOGE("Invalid cardName!");
        return HDF_FAILURE;
    }

    cardInfo = FindCardIns(cardName);
    if (cardInfo == NULL) {
        cardInfo = AddCardIns(cardName);
        if (cardInfo == NULL) {
            AUDIO_FUNC_LOGE("AddCardIns failed!");
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

struct AudioCardInfo *GetCardIns(const char *cardName)
{
    if (cardName == NULL) {
        AUDIO_FUNC_LOGE("Invalid cardName!");
        return NULL;
    }

    return FindCardIns(cardName);
}

void CheckCardStatus(struct AudioCardInfo *cardIns)
{
    int32_t ret;
    if (cardIns == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty!");
        return;
    }
    if (cardIns->cardStatus > 0) {
        cardIns->cardStatus -= 1;
    }
    if (cardIns->cardStatus == 0) {
        if (cardIns->renderPcmHandle != NULL) {
            ret = snd_pcm_close(cardIns->renderPcmHandle);
            if (ret < 0) {
                AUDIO_FUNC_LOGE("snd_pcm_close fail: %{public}s", snd_strerror(ret));
            }
            cardIns->renderPcmHandle = NULL;
        }
        if (cardIns->capturePcmHandle != NULL) {
            ret = snd_pcm_close(cardIns->capturePcmHandle);
            if (ret < 0) {
                AUDIO_FUNC_LOGE("snd_pcm_close fail: %{public}s", snd_strerror(ret));
            }
            cardIns->capturePcmHandle = NULL;
        }
        (void)memset_s(cardIns->cardName, MAX_CARD_NAME_LEN + 1, 0, MAX_CARD_NAME_LEN + 1);
    }
}

static void CardInfoRelease(void)
{
    for (int i = 0; i < SND_CARD_MAX; i++) {
        for (int j = 0; j < AUDIO_MAX_CARD_NUM; j++) {
            if (g_sndCardList[i][j] == NULL) {
                continue;
            }
            if (g_sndCardList[i][j]->ctlRenderNameList != NULL) {
                AudioMemFree((void **)&(g_sndCardList[i][j]->ctlRenderNameList));
            }
            if (g_sndCardList[i][j]->ctlCaptureNameList != NULL) {
                AudioMemFree((void **)&(g_sndCardList[i][j]->ctlCaptureNameList));
            }
            AudioMemFree((void **)&(g_sndCardList[i][j]));
        }
    }
    g_parseFlag = false;
}

int32_t DestroyCardList(void)
{
    int32_t i;

    if (g_audioCardIns != NULL) {
        for (i = 0; i < MAX_CARD_NUM; i++) {
            if (g_audioCardIns[i].cardStatus != 0) {
                AUDIO_FUNC_LOGE("refCount is not zero, Sound card in use!");
                return HDF_ERR_DEVICE_BUSY;
            }
        }
        AudioMemFree((void **)&g_audioCardIns);
        g_audioCardIns = NULL;

        /* Release the sound card configuration space */
        CardInfoRelease();
    }

    return HDF_SUCCESS;
}

static int32_t GetDevIns(struct AlsaCardsList *cardIns, int card, const char *cardId, int dev, const char *pcmInfoId)
{
    int32_t i;
    int32_t ret;

    if (cardIns == NULL || cardId == NULL || pcmInfoId == NULL || strlen(cardId) == 0) {
        AUDIO_FUNC_LOGE("The parameter is empty!");
        return HDF_FAILURE;
    }

    for (i = 0; i < MAX_CARD_NUM; i++) {
        if (strlen(cardIns->alsaDevIns[i].cardId) == 0) {
            cardIns->alsaDevIns[i].card = card;
            cardIns->alsaDevIns[i].device = dev;
            ret = strncpy_s(cardIns->alsaDevIns[i].cardId, MAX_CARD_NAME_LEN + 1, cardId, strlen(cardId));
            if (ret != 0) {
                AUDIO_FUNC_LOGE("strncpy_s failed!");
                return HDF_FAILURE;
            }
            ret = strncpy_s(cardIns->alsaDevIns[i].pcmInfoId, MAX_CARD_NAME_LEN + 1, pcmInfoId, strlen(pcmInfoId));
            if (ret != 0) {
                AUDIO_FUNC_LOGE("strncpy_s failed!");
                return HDF_FAILURE;
            }
            return HDF_SUCCESS;
        }
    }
    AUDIO_FUNC_LOGE("A maximum of %{public}d sound cards are supported!", MAX_CARD_NUM);

    return HDF_FAILURE;
}

int32_t CloseMixerHandle(snd_mixer_t *alsaMixHandle)
{
    int32_t ret;

    if (alsaMixHandle == NULL) {
        return HDF_SUCCESS;
    }

    ret = snd_mixer_close(alsaMixHandle);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("mixer close error: %{public}s.", snd_strerror(ret));
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

void InitSound(snd_mixer_t **mixer, char *hwCtlName)
{
    int32_t ret;

    if (mixer == NULL || hwCtlName == NULL) {
        AUDIO_FUNC_LOGE("The parameter is null.");
        return;
    }

    ret = snd_mixer_open(mixer, 0);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to open mixer: %{public}s.", snd_strerror(ret));
        return;
    }

    ret = snd_mixer_attach(*mixer, hwCtlName);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to attach mixer: %{public}s.", snd_strerror(ret));
        ret = snd_mixer_close(*mixer);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("mixer close error: %{public}s.", snd_strerror(ret));
        }
        return;
    }

    ret = snd_mixer_selem_register(*mixer, NULL, NULL);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to register mixer element: %{public}s.", snd_strerror(ret));
        ret = snd_mixer_close(*mixer);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("mixer close error: %{public}s.", snd_strerror(ret));
        }
        return;
    }

    ret = snd_mixer_load(*mixer);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to load mixer element: %{public}s.", snd_strerror(ret));
        ret = snd_mixer_close(*mixer);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("mixer close error: %{public}s.", snd_strerror(ret));
        }
        return;
    }
}

struct HdfIoService *HdfIoServiceBindName(const char *serviceName)
{
    (void)serviceName;
    /* Nothing to do */
    static struct HdfIoService hdfIoService;
    return &hdfIoService;
}

static void GetDevCardsInfo(snd_ctl_t *handle, snd_ctl_card_info_t *info, snd_pcm_info_t *pcmInfo,
    struct AlsaCardsList *cardIns, struct CardStream cardStream)
{
    int dev = -1;
    int32_t ret;

    if (handle == NULL || info == NULL || pcmInfo == NULL || cardIns == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return;
    }

    while (1) {
        ret = snd_ctl_pcm_next_device(handle, &dev);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("snd_ctl_pcm_next_device error: %{public}s.", snd_strerror(ret));
        }
        if (dev < 0) {
            break;
        }

        snd_pcm_info_set_device(pcmInfo, dev);
        snd_pcm_info_set_subdevice(pcmInfo, 0);
        snd_pcm_info_set_stream(pcmInfo, cardStream.stream);
        if ((ret = snd_ctl_pcm_info(handle, pcmInfo)) < 0) {
            if (ret != -ENOENT) {
                AUDIO_FUNC_LOGE(
                    "control digital audio info (%{public}d): %{public}s", cardStream.card, snd_strerror(ret));
            }
            continue;
        }

        const char *cardId = snd_ctl_card_info_get_id(info);
        const char *pcmInfoId = snd_pcm_info_get_id(pcmInfo);
        ret = GetDevIns(cardIns, cardStream.card, cardId, dev, pcmInfoId);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("GetDevIns error.");
            return;
        }
    }
}

static void GetDeviceList(struct AlsaCardsList *cardIns, snd_pcm_stream_t stream)
{
    int32_t ret;
    snd_ctl_t *handle;
    int card = -1;
    snd_ctl_card_info_t *info = NULL;
    snd_pcm_info_t *pcminfo = NULL;
    char deviceName[MAX_CARD_NAME_LEN] = {0};
    struct CardStream cardStream;

    if (cardIns == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return;
    }
    snd_ctl_card_info_alloca(&info);
    snd_pcm_info_alloca(&pcminfo);

    ret = snd_card_next(&card);
    if (ret < 0 || card < 0) {
        AUDIO_FUNC_LOGE("No soundcards found: %{public}s.", snd_strerror(ret));
        return;
    }
    while (card >= 0) {
        (void)memset_s(deviceName, MAX_CARD_NAME_LEN, 0, MAX_CARD_NAME_LEN);
        ret = snprintf_s(deviceName, MAX_CARD_NAME_LEN, MAX_CARD_NAME_LEN - 1, "hw:%d", card);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("snprintf_s failed");
            return;
        }

        ret = snd_ctl_open(&handle, deviceName, 0);
        if (ret == HDF_SUCCESS) {
            ret = snd_ctl_card_info(handle, info);
            if (ret == HDF_SUCCESS) {
                cardStream.card = card;
                cardStream.stream = stream;
                GetDevCardsInfo(handle, info, pcminfo, cardIns, cardStream);
            }
            ret = snd_ctl_close(handle);
            if (ret < 0) {
                AUDIO_FUNC_LOGE("mixer close error: %{public}s.", snd_strerror(ret));
                return;
            }
        }

        ret = snd_card_next(&card);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("snd_card_next error: %{public}s", snd_strerror(ret));
            return;
        }
    }
}

int32_t GetSelCardInfo(struct AudioCardInfo *cardIns, struct AlsaDevInfo *devInsHandle)
{
    int32_t ret;

    if (cardIns == NULL || devInsHandle == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    ret = snprintf_s(cardIns->devName, MAX_CARD_NAME_LEN, MAX_CARD_NAME_LEN - 1, "plughw:%d,%d", devInsHandle->card,
        devInsHandle->device);
    if (ret >= 0) {
        ret = snprintf_s(cardIns->ctrlName, MAX_CARD_NAME_LEN, MAX_CARD_NAME_LEN - 1, "hw:%d", devInsHandle->card);
        if (ret >= 0) {
            ret = snprintf_s(cardIns->alsaCardId, MAX_CARD_NAME_LEN, MAX_CARD_NAME_LEN - 1, "%s", devInsHandle->cardId);
            if (ret >= 0) {
                return HDF_SUCCESS;
            }
        }
    }
    AUDIO_FUNC_LOGE("snprintf_s failed");

    return HDF_FAILURE;
}

static struct DevProcInfo *MatchPimarySoundCard(struct DevProcInfo *cardInfo[], const char *adapterName)
{
    if (cardInfo == NULL || adapterName == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return NULL;
    }

    if (strncmp(adapterName, PRIMARY, strlen(PRIMARY)) != 0) {
        AUDIO_FUNC_LOGE("The user sound card name %{public}s is incorrect!", adapterName);
        return NULL;
    }

    for (int i = 0; i < AUDIO_MAX_CARD_NUM; i++) {
        if (cardInfo[i] != NULL) {
            if (strncmp(cardInfo[i]->cardName, adapterName, strlen(adapterName)) == 0) {
                return cardInfo[i];
            }
        }
    }
    AUDIO_FUNC_LOGE("No sound card selected by the user is matched from the configuration file.");

    return NULL;
}

static struct DevProcInfo *MatchHdmiSoundCard(struct DevProcInfo *cardInfo[], const char *adapterName)
{
    if (cardInfo == NULL || adapterName == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return NULL;
    }

    if (strncmp(adapterName, HDMI, strlen(HDMI)) != 0) {
        AUDIO_FUNC_LOGE("The user sound card name %{public}s is incorrect!", adapterName);
        return NULL;
    }

    for (int i = 0; i < AUDIO_MAX_CARD_NUM; i++) {
        if (cardInfo[i] != NULL) {
            if (strncmp(cardInfo[i]->cardName, HDMI, strlen(HDMI)) == 0) {
                return cardInfo[i];
            }
        }
    }
    AUDIO_FUNC_LOGE("No sound card selected by the user is matched from the configuration file.");

    return NULL;
}

static int32_t GetPrimaryCardInfo(struct AudioCardInfo *cardIns, struct AlsaCardsList *alsaCardsList, int32_t alsaDevNum)
{
    int32_t i;
    int32_t ret;
    struct DevProcInfo *primaryInfo = NULL;
    struct DevProcInfo **cardInfo = NULL;

    if (cardIns == NULL || alsaCardsList == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    cardInfo = AudioGetSoundCardsInfo(SND_CARD_PRIMARY);
    if (cardInfo == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    primaryInfo = MatchPimarySoundCard(cardInfo, cardIns->cardName);
    if (primaryInfo == NULL) {
        AUDIO_FUNC_LOGE("get cardId is null.");
        return HDF_FAILURE;
    }

    for (i = 0; i < MAX_CARD_NUM; i++) {
        /** Built in codec */
        if (strcmp(primaryInfo->cid, alsaCardsList->alsaDevIns[i].cardId) == 0 
            && (alsaCardsList->alsaDevIns[i].device == alsaDevNum)) {
            ret = GetSelCardInfo(cardIns, &alsaCardsList->alsaDevIns[i]);
            if (ret != HDF_SUCCESS) {
                AUDIO_FUNC_LOGE("GetSelCardInfo error.");
            }
            return ret;
        }
    }

    return HDF_FAILURE;
}

static int32_t GetUsbCardInfo(struct AudioCardInfo *cardIns, struct AlsaCardsList *alsaCardsList)
{
    int32_t i;
    int32_t ret;

    if (cardIns == NULL || alsaCardsList == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    for (i = 0; i < MAX_CARD_NUM; i++) {
        /** External codec */
        if (strcmp(USB_AUDIO, alsaCardsList->alsaDevIns[i].pcmInfoId) == 0) {
            ret = GetSelCardInfo(cardIns, &alsaCardsList->alsaDevIns[i]);
            if (ret != HDF_SUCCESS) {
                AUDIO_FUNC_LOGE("GetSelCardInfo error.");
            }
            return ret;
        }
    }

    return HDF_FAILURE;
}

static int32_t GetHdmiCardInfo(struct AudioCardInfo *cardIns, struct AlsaCardsList *alsaCardsList, int32_t alsaDevNum)
{
    int32_t i;
    int32_t ret;
    struct DevProcInfo *hdmiInfo = NULL;
    struct DevProcInfo **hdmiCardInfo = NULL;

    if (cardIns == NULL || alsaCardsList == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    hdmiCardInfo = AudioGetSoundCardsInfo(SND_CARD_HDMI);
    if (hdmiCardInfo == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    hdmiInfo = MatchHdmiSoundCard(hdmiCardInfo, cardIns->cardName);
    if (hdmiInfo == NULL) {
        AUDIO_FUNC_LOGE("get cardId is null.");
        return HDF_FAILURE;
    }
    for (i = 0; i < MAX_CARD_NUM; i++) {
        /** hdmi codec */
        if (strcmp(hdmiInfo->cid, alsaCardsList->alsaDevIns[i].cardId) == 0 
            && (alsaCardsList->alsaDevIns[i].device == alsaDevNum)) {
            ret = GetSelCardInfo(cardIns, &alsaCardsList->alsaDevIns[i]);
            if (ret != HDF_SUCCESS) {
                AUDIO_FUNC_LOGE("GetSelCardInfo error.");
            }
            return ret;
        }
    }

    AUDIO_FUNC_LOGE("get hdmi cardId fail.");
    return HDF_FAILURE;
}

int32_t MatchSelAdapter(const char *adapterName, struct AudioCardInfo *cardIns, int32_t alsaDevNum)
{
    if (adapterName == NULL || cardIns == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    if (strncmp(adapterName, PRIMARY, strlen(PRIMARY)) == 0) {
        return GetPrimaryCardInfo(cardIns, &g_alsaCardsList, alsaDevNum);
    } else if (strncmp(adapterName, USB, strlen(USB)) == 0) {
        return GetUsbCardInfo(cardIns, &g_alsaCardsList);
    } else if (strncmp(adapterName, HDMI, strlen(HDMI)) == 0) {
        return GetHdmiCardInfo(cardIns, &g_alsaCardsList, alsaDevNum);
    } else if (strncmp(adapterName, A2DP, strlen(A2DP)) == 0) {
        AUDIO_FUNC_LOGE("Currently not supported A2DP, please check!");
        return HDF_ERR_NOT_SUPPORT;
    } else {
        AUDIO_FUNC_LOGE("The selected sound card not find, please check!");
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

static snd_mixer_elem_t *AudioUsbFindElement(snd_mixer_t *mixer, snd_pcm_stream_t stream)
{
    int i;
    int count;
    int32_t maxLoop = MAX_ELEMENT;
    snd_mixer_elem_t *element = NULL;
    char *mixerCtlName = NULL;
    char **usbCtlVol = NULL;
    if (mixer == NULL) {
        AUDIO_FUNC_LOGE("The parameter is NULL!");
        return NULL;
    }

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        usbCtlVol = g_usbSimpleRenderCtlVol;
        count = sizeof(g_usbSimpleRenderCtlVol) / sizeof(char *);
    } else {
        usbCtlVol = g_usbSimpleCapCtlVol;
        count = sizeof(g_usbSimpleCapCtlVol) / sizeof(char *);
    }

    for (element = snd_mixer_first_elem(mixer); element != NULL && maxLoop >= 0;
         element = snd_mixer_elem_next(element)) {
        for (i = 0; i < count; i++) {
            mixerCtlName = usbCtlVol[i];
            /* Compare whether the element name is the option we want to set */
            if (strcmp(mixerCtlName, snd_mixer_selem_get_name(element)) == 0) {
                return element;
            }
        }
        maxLoop--;
    }
    return NULL;
}

static snd_mixer_elem_t *AudioFindElement(const char *mixerCtlName, snd_mixer_t *mixer)
{
    int32_t maxLoop = MAX_ELEMENT;

    if (mixerCtlName == NULL || mixer == NULL) {
        AUDIO_FUNC_LOGE("The parameter is NULL!");
        return NULL;
    }
    snd_mixer_elem_t *element = snd_mixer_first_elem(mixer);
    if (element == NULL) {
        AUDIO_FUNC_LOGE("snd_mixer_first_elem failed.");
        return NULL;
    }
    while (element && maxLoop >= 0) {
        /* Compare whether the element name is the option we want to set */
        if (strcmp(mixerCtlName, snd_mixer_selem_get_name(element)) == 0) {
            break;
        }
        /* If not, keep looking for the next one */
        element = snd_mixer_elem_next(element);
        maxLoop--;
    }

    if (element == NULL || maxLoop < 0) {
        AUDIO_FUNC_LOGE("snd_mixer_find_selem Err\n");
        return NULL;
    }

    return element;
}

int32_t GetPriMixerCtlElement(struct AudioCardInfo *cardIns, snd_mixer_t *mixer, snd_pcm_stream_t stream)
{
    int32_t i;
    struct DevProcInfo *primaryInfo = NULL;
    struct DevProcInfo **cardInfo = NULL;

    if (cardIns == NULL || mixer == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    cardInfo = AudioGetSoundCardsInfo(SND_CARD_PRIMARY);
    if (cardInfo == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    primaryInfo = MatchPimarySoundCard(cardInfo, cardIns->cardName);
    if (primaryInfo == NULL) {
        AUDIO_FUNC_LOGE("get cardId is null.");
        return HDF_FAILURE;
    }

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        cardIns->volElemCount = primaryInfo->ctlRenderVolNameCount;
        cardIns->volElemList =
            (struct AlsaMixerElem *)OsalMemCalloc(cardIns->volElemCount * sizeof(struct AlsaMixerElem));
        if (cardIns->volElemList == NULL) {
            AUDIO_FUNC_LOGE("PLAYBACK cardIns->volElemList Out of memory!");
            return HDF_ERR_MALLOC_FAIL;
        }
        for (i = 0; i < (int32_t)cardIns->volElemCount; i++) {
            cardIns->volElemList[i].elem = AudioFindElement(primaryInfo->ctlRenderNameList[i].name, mixer);
        }
    } else {
        cardIns->volElemCount = primaryInfo->ctlCaptureVolNameCount;
        cardIns->volElemList =
            (struct AlsaMixerElem *)OsalMemCalloc(cardIns->volElemCount * sizeof(struct AlsaMixerElem));
        if (cardIns->volElemList == NULL) {
            AUDIO_FUNC_LOGE("CAPTURE cardIns->volElemList Out of memory!");
            return HDF_ERR_MALLOC_FAIL;
        }
        for (i = 0; i < (int32_t)cardIns->volElemCount; i++) {
            cardIns->volElemList[i].elem = AudioFindElement(primaryInfo->ctlCaptureNameList[i].name, mixer);
        }
    }

    return HDF_SUCCESS;
}

static int32_t AudioRenderSetVolumeRange(snd_mixer_elem_t *ctlElem)
{
    int32_t ret;
    long volMin = -1;
    long volMax = -1;
    if (ctlElem == NULL) {
        AUDIO_FUNC_LOGE("ctlElem is NULL.");
        return HDF_FAILURE;
    }
    ret = snd_mixer_selem_has_playback_volume(ctlElem);
    /* ret is 0 if no control is present, 1 if it's present */
    if (ret == 0) {
        AUDIO_FUNC_LOGE("reason: %{public}s", snd_strerror(ret));
        return HDF_SUCCESS;
    }
    ret = snd_mixer_selem_get_playback_volume_range(ctlElem, &volMin, &volMax);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to get playback volume range: %{public}s", snd_strerror(ret));
        return HDF_FAILURE;
    }

    if (volMin == MIN_VOLUME && volMax == MAX_VOLUME) {
        return HDF_SUCCESS;
    } else {
        ret = snd_mixer_selem_set_playback_volume_range(ctlElem, MIN_VOLUME, MAX_VOLUME);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("Failed to set playback volume range: %{public}s", snd_strerror(ret));
            return HDF_FAILURE;
        }
    }
    return HDF_SUCCESS;
}

static int32_t AudioCaptureSetVolumeRange(snd_mixer_elem_t *ctlElem)
{
    int32_t ret;
    long volMin = -1;
    long volMax = -1;
    if (ctlElem == NULL) {
        AUDIO_FUNC_LOGE("ctlElem is NULL.");
        return HDF_FAILURE;
    }
    ret = snd_mixer_selem_has_capture_volume(ctlElem);
    /* ret is 0 if no control is present, 1 if it's present */
    if (ret == 0) {
        AUDIO_FUNC_LOGE("reason: %{public}s", snd_strerror(ret));
        return HDF_SUCCESS;
    }
    ret = snd_mixer_selem_get_capture_volume_range(ctlElem, &volMin, &volMax);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Failed to get capture volume range: %{public}s", snd_strerror(ret));
        return HDF_FAILURE;
    }

    if (volMin != MIN_VOLUME || volMax != MAX_VOLUME) {
        ret = snd_mixer_selem_set_capture_volume_range(ctlElem, MIN_VOLUME, MAX_VOLUME);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("Failed to set capture volume range: %{public}s", snd_strerror(ret));
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static int32_t InitMixerCtrlVolumeRange(snd_mixer_elem_t *ctlElem, snd_pcm_stream_t stream)
{
    if (ctlElem == NULL) {
        AUDIO_FUNC_LOGE("ctlElem is NULL.");
        return HDF_FAILURE;
    }

    switch (stream) {
        case SND_PCM_STREAM_PLAYBACK:
            return AudioRenderSetVolumeRange(ctlElem);
        case SND_PCM_STREAM_CAPTURE:
            return AudioCaptureSetVolumeRange(ctlElem);
        default:
            AUDIO_FUNC_LOGE("stream is error!");
            break;
    }

    return HDF_FAILURE;
}

int32_t AudioSetCtrlVolumeRange(struct AudioCardInfo *cardIns, const char *adapterName, snd_pcm_stream_t stream)
{
    if (cardIns == NULL || adapterName == NULL) {
        AUDIO_FUNC_LOGE("Parameter error!");
        return HDF_FAILURE;
    }

    if (strncmp(adapterName, PRIMARY, strlen(PRIMARY)) == 0) {
        for (int32_t i = 0; i < (int32_t)cardIns->volElemCount; i++) {
            if (InitMixerCtrlVolumeRange(cardIns->volElemList[i].elem, stream) != HDF_SUCCESS) {
                AUDIO_FUNC_LOGE("set valume failed!");
                return HDF_FAILURE;
            }
        }
        return HDF_SUCCESS;
    }

    if (strncmp(adapterName, USB, strlen(USB)) == 0) {
        return InitMixerCtrlVolumeRange(cardIns->usbCtlVolume, stream);
    }

    if (strncmp(adapterName, HDMI, strlen(HDMI)) == 0) {
        AUDIO_FUNC_LOGW("HDMI not ctlElement.");
        return HDF_SUCCESS;
    }

    AUDIO_FUNC_LOGE("This type of sound card: %{public}s is not supported temporarily!", adapterName);
    return HDF_FAILURE;
}

int32_t InitMixerCtlElement(
    const char *adapterName, struct AudioCardInfo *cardIns, snd_mixer_t *mixer, snd_pcm_stream_t stream)
{
    int32_t ret;

    if (adapterName == NULL || cardIns == NULL || mixer == NULL) {
        AUDIO_FUNC_LOGE("Parameter error!");
        return HDF_FAILURE;
    }

    if (strncmp(adapterName, PRIMARY, strlen(PRIMARY)) == 0) {
        ret = GetPriMixerCtlElement(cardIns, mixer, stream);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("Render GetPriMixerCtlElement failed.");
            return HDF_FAILURE;
        }
    } else if (strncmp(adapterName, USB, strlen(USB)) == 0) {
        cardIns->usbCtlVolume = AudioUsbFindElement(mixer, stream);
    } else if (strncmp(adapterName, HDMI, strlen(HDMI)) == 0) {
        AUDIO_FUNC_LOGI("HDMI not ctlElement.");
        return HDF_SUCCESS;
    } else {
        AUDIO_FUNC_LOGE("The selected sound card not supported, please check!");
        return HDF_FAILURE;
    }

    ret = AudioSetCtrlVolumeRange(cardIns, adapterName, stream);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGI("AudioSetCtrlVolumeRange failed.");
        return ret;
    }

    return HDF_SUCCESS;
}

struct AudioCardInfo *AudioGetCardInstance(const char *adapterName)
{
    int32_t ret;

    if (adapterName == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return NULL;
    }

    ret = InitCardIns();
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("Failed to initialize sound card information.");
        return NULL;
    }

    ret = AudioAddCardIns(adapterName);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("AudioAddCardIns failed.");
        return NULL;
    }

    return GetCardIns(adapterName);
}

int32_t AudioGetCardInfo(struct AudioCardInfo *cardIns, const char *adapterName, snd_pcm_stream_t stream)
{
    int32_t alsaDevNum = 0;

    if (cardIns == NULL || adapterName == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return HDF_FAILURE;
    }

    /* Parse sound card from configuration file */
    if (CardInfoParseFromConfig() != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("parse config file failed!");
        return HDF_FAILURE;
    }
    /* Read sound card list from alsa hardware */
    (void)memset_s(&g_alsaCardsList, sizeof(struct AlsaCardsList), 0, sizeof(struct AlsaCardsList));
    GetDeviceList(&g_alsaCardsList, stream);

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        alsaDevNum = 0;
    } else if (stream == SND_PCM_STREAM_CAPTURE) {
        alsaDevNum = 0;
    }

    if (MatchSelAdapter(adapterName, cardIns, alsaDevNum) < 0) {
        AUDIO_FUNC_LOGE("MatchSelAdapter is error.");
        return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

void AudioMixerCtlElementWrite(const char *mixerCtrlName, const char *item)
{
    int32_t ret;
    snd_ctl_t *handle = NULL;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *control;
    snd_hctl_t *hctl;
    snd_hctl_elem_t *elem;
    char card[64] = "default";

    if (mixerCtrlName == NULL) {
        AUDIO_FUNC_LOGE("AudioCtlRenderSetVolume parameter is NULL!");
        return ;
    }

    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&control);

    if (snd_ctl_ascii_elem_id_parse(id, mixerCtrlName)) {
        AUDIO_FUNC_LOGE("Wrong control identifier: %{public}s\n", mixerCtrlName);
        return ;
    }

    if ((ret = snd_ctl_open(&handle, card, 0)) < 0) {
        AUDIO_FUNC_LOGE("Control %{public}s open error: %{public}s\n", card, snd_strerror(ret));
        return ;
    }

    snd_ctl_elem_info_set_id(info, id);
    if ((ret = snd_ctl_elem_info(handle, info)) < 0) {
        AUDIO_FUNC_LOGE("Cannot find the given element from control %{public}s\n", card);
        return ;
    }

    snd_ctl_elem_info_get_id(info, id);     /* FIXME: Remove it when hctl find works ok !!! */
    snd_ctl_elem_value_set_id(control, id);
    if ((ret = snd_ctl_elem_read(handle, control)) < 0) {
        AUDIO_FUNC_LOGE("Cannot read the given element from control %{public}s\n", card);
        return ;
    }

    ret = snd_ctl_ascii_value_parse(handle, control, info, item);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("Control %{public}s parse error: %{public}s\n", card, snd_strerror(ret));
        return ;
    }

    if ((ret = snd_ctl_elem_write(handle, control)) < 0) {
        AUDIO_FUNC_LOGE("Control %{public}s element write error: %{public}s\n", card, snd_strerror(ret));
        return ;
    }

    if ((ret = snd_hctl_open(&hctl, card, 0)) < 0) {
        AUDIO_FUNC_LOGE("Control %{public}s open error: %{public}s\n", card, snd_strerror(ret));
        return ;
    }

    if ((ret = snd_hctl_load(hctl)) < 0) {
        AUDIO_FUNC_LOGE("Control %{public}s load error: %{public}s\n", card, snd_strerror(ret));
        return ;
    }

    elem = snd_hctl_find_elem(hctl, id);
    if (elem == NULL) {
        AUDIO_FUNC_LOGE("Could not find the specified element\n");
    }

    snd_hctl_close(hctl);

    snd_ctl_close(handle);
    handle = NULL;

    return ;
}

int32_t CheckParaFormat(struct AudioPcmHwParams hwParams, snd_pcm_format_t *alsaPcmFormat)
{
    if (alsaPcmFormat == NULL) {
        AUDIO_FUNC_LOGE("paras is NULL!");
        return HDF_FAILURE;
    }
    enum AudioFormat audioFormat = hwParams.format;
    bool isBigEndian = hwParams.isBigEndian;

    /** Little Endian */
    if (!isBigEndian) {
        switch (audioFormat) {
            case AUDIO_FORMAT_TYPE_PCM_8_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S8; /** Signed 8 bit */
                break;
            case AUDIO_FORMAT_TYPE_PCM_16_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S16_LE; /** Signed 16 bit Little Endian */
                break;
            case AUDIO_FORMAT_TYPE_PCM_24_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S24_LE; /** Signed 24 bit Little Endian */
                break;
            case AUDIO_FORMAT_TYPE_PCM_32_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S32_LE; /** Signed 32 bit Little Endian */
                break;
            default:
                return HDF_ERR_NOT_SUPPORT;
        }
    } else { /** Big Endian */
        switch (audioFormat) {
            case AUDIO_FORMAT_TYPE_PCM_8_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S8; /** Signed 8 bit */
                break;
            case AUDIO_FORMAT_TYPE_PCM_16_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S16_BE; /** Signed 16 bit Big Endian */
                break;
            case AUDIO_FORMAT_TYPE_PCM_24_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S24_BE; /** Signed 24 bit Big Endian */
                break;
            case AUDIO_FORMAT_TYPE_PCM_32_BIT:
                *alsaPcmFormat = SND_PCM_FORMAT_S32_BE; /** Signed 32 bit Big Endian */
                break;
            default:
                return HDF_ERR_NOT_SUPPORT;
        }
    }

    return HDF_SUCCESS;
}

static char *AudioAdaptersGetConfig(const char *fpath)
{
    int32_t jsonStrSize;
    FILE *fp = NULL;
    char *pJsonStr = NULL;
    char pathBuf[PATH_MAX] = {0};

    if (fpath == NULL) {
        AUDIO_FUNC_LOGE("Parameter is null!!!");
        return NULL;
    }
    if (realpath(fpath, pathBuf) == NULL) {
        AUDIO_FUNC_LOGE("File path invalid!");
        return NULL;
    }

    fp = fopen(pathBuf, "r");
    if (fp == NULL) {
        AUDIO_FUNC_LOGE("Can not open config file [ %{public}s ].", fpath);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        AUDIO_FUNC_LOGE("fseek configuration file error!");
        (void)fclose(fp);
        return NULL;
    }
    jsonStrSize = ftell(fp);
    if (jsonStrSize <= 0) {
        AUDIO_FUNC_LOGE("The configuration file size <= 0!");
        (void)fclose(fp);
        return NULL;
    }
    rewind(fp);
    if (jsonStrSize > ALSA_CONFIG_FILE_MAX) {
        AUDIO_FUNC_LOGE("The configuration file is too large to load!");
        (void)fclose(fp);
        return NULL;
    }
    pJsonStr = (char *)OsalMemCalloc((uint32_t)jsonStrSize + 1);
    if (pJsonStr == NULL) {
        AUDIO_FUNC_LOGE("OsalMemCalloc pJsonStr failed!");
        (void)fclose(fp);
        return NULL;
    }
    if (fread(pJsonStr, jsonStrSize, 1, fp) != 1) {
        AUDIO_FUNC_LOGE("Read to config file failed!!!");
        (void)fclose(fp);
        AudioMemFree((void **)&pJsonStr);
        return NULL;
    }
    (void)fclose(fp);

    return pJsonStr;
}

static cJSON *AudioCardGetConfig(const char *fpath)
{
    char *pJsonStr = NULL;
    cJSON *cJsonObj = NULL;

    if (fpath == NULL) {
        AUDIO_FUNC_LOGE("Parameter error!");
        return NULL;
    }

    pJsonStr = AudioAdaptersGetConfig(fpath);
    if (pJsonStr == NULL) {
        AUDIO_FUNC_LOGE("AudioAdaptersGetConfig failed!");
        return NULL;
    }

    cJsonObj = cJSON_Parse(pJsonStr);
    if (cJsonObj == NULL) {
        AUDIO_FUNC_LOGE("AudioAdaptersGetConfig failed!");
        AudioMemFree((void **)&pJsonStr);
        return NULL;
    }
    AudioMemFree((void **)&pJsonStr);

    return cJsonObj;
}

static int32_t AudioAdapterCheckName(const char *name)
{
    uint32_t len;
    const char *strName = name;

    if (strName == NULL) {
        AUDIO_FUNC_LOGE("Invalid parameter!");
        return HDF_FAILURE;
    }

    len = strlen(strName);
    if (len == 0 || len >= CARD_ID_LEN_MAX) {
        AUDIO_FUNC_LOGW("name len is zero or too long!, len = %{public}d", len);
        return HDF_FAILURE;
    }

    if (!isalpha(*strName++)) { // Names must begin with a letter
        AUDIO_FUNC_LOGE("The name of the illegal!");
        return HDF_FAILURE;
    }

    while (*strName != '\0') {
        if (*strName == '_' || *strName == '-') {
            strName++;
            continue;
        }

        if (!isalnum(*strName++)) {
            AUDIO_FUNC_LOGE("The name of the illegal!");
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static enum SndCardType AudioAdapterNameToType(const char *name)
{
    enum SndCardType cardType = SND_CARD_UNKNOWN;

    if (name == NULL) {
        AUDIO_FUNC_LOGE("Invalid parameter!");
        return SND_CARD_UNKNOWN;
    }

    if (strcmp(name, "primary") == 0) {
        cardType = SND_CARD_PRIMARY;
    } else if (strcmp(name, "hdmi") == 0) {
        cardType = SND_CARD_HDMI;
    } else if (strcmp(name, "usb") == 0) {
        cardType = SND_CARD_USB;
    } else if (strcmp(name, "bt") == 0) {
        cardType = SND_CARD_BT;
    }

    return cardType;
}

static int32_t AudioGetVolumeJsonObj(cJSON *object, cJSON **volumeJsonObj, const char *ctlName)
{
    if (object == NULL || volumeJsonObj == NULL || ctlName == NULL) {
        AUDIO_FUNC_LOGE("Parameters is NULL!");
        return HDF_FAILURE;
    }

    cJSON *ctlObjList = cJSON_GetObjectItem(object, ctlName);
    if (ctlObjList == NULL) {
        AUDIO_FUNC_LOGE("secondItem[%{public}s] Get Object Fail!", ctlName);
        return HDF_FAILURE;
    }
    *volumeJsonObj = ctlObjList;
    return HDF_SUCCESS;
}

static int32_t AudioGetRenderVolumeInstance(struct DevProcInfo *adapter, cJSON *ctlVolumeList)
{
    int32_t ret;
    cJSON *listSub = NULL;
    char *ctlName = NULL;
    cJSON *nodeCtlName = NULL;

    if (adapter == NULL || ctlVolumeList == NULL) {
        AUDIO_FUNC_LOGE("Parameter is null");
        return HDF_FAILURE;
    }

    int32_t arraySize = cJSON_GetArraySize(ctlVolumeList);
    if (arraySize < 0 || arraySize > MIXER_CTL_MAX_NUM) {
        AUDIO_FUNC_LOGE("Maximum support: %{public}d, but actually is %{public}d.", MIXER_CTL_MAX_NUM, arraySize);
        return HDF_FAILURE;
    }

    adapter->ctlRenderVolNameCount = arraySize;
    adapter->ctlRenderNameList =
        (struct MixerCtlVolumeName *)OsalMemCalloc(arraySize * sizeof(struct MixerCtlVolumeName));
    if (adapter->ctlRenderNameList == NULL) {
        AUDIO_FUNC_LOGE("Out of memory!");
        return HDF_ERR_MALLOC_FAIL;
    }

    for (int32_t index = 0; index < arraySize; index++) {
        listSub = cJSON_GetArrayItem(ctlVolumeList, index);
        if (listSub == NULL) {
            AUDIO_FUNC_LOGE("cJSON_GetArrayItem failed, listSub is NULL!");
            AudioMemFree((void **)&adapter->ctlRenderNameList);
            return HDF_FAILURE;
        }
        if ((nodeCtlName = cJSON_GetObjectItem(listSub, "name")) == NULL) {
            AudioMemFree((void **)&adapter->ctlRenderNameList);
            AUDIO_FUNC_LOGE("cJSON_GetObjectItem Failed!");
            return HDF_FAILURE;
        }
        ctlName = nodeCtlName->valuestring;
        ret = strncpy_s(adapter->ctlRenderNameList[index].name, ALSA_CTL_NAME_LEN - 1, ctlName, strlen(ctlName));
        if (ret != 0) {
            AUDIO_FUNC_LOGE("strncpy_s failed!");
            AudioMemFree((void **)&adapter->ctlRenderNameList);
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static int32_t AudioGetCaptureVolumeInstance(struct DevProcInfo *adapter, cJSON *ctlCaptureVolList)
{
    int32_t ret;
    cJSON *listSub = NULL;
    char *ctlName = NULL;
    cJSON *nodeCtlName = NULL;

    if (adapter == NULL || ctlCaptureVolList == NULL) {
        AUDIO_FUNC_LOGE("Parameter is null");
        return HDF_FAILURE;
    }
    int32_t arraySize = cJSON_GetArraySize(ctlCaptureVolList);
    if (arraySize < 0 || arraySize > MIXER_CTL_MAX_NUM) {
        AUDIO_FUNC_LOGE("Maximum support: %{public}d, but actually is %{public}d.", MIXER_CTL_MAX_NUM, arraySize);
        return HDF_FAILURE;
    }
    adapter->ctlCaptureVolNameCount = arraySize;
    adapter->ctlCaptureNameList =
        (struct MixerCtlVolumeName *)OsalMemCalloc(arraySize * sizeof(struct MixerCtlVolumeName));
    if (adapter->ctlCaptureNameList == NULL) {
        AUDIO_FUNC_LOGE("Out of memory!");
        return HDF_ERR_MALLOC_FAIL;
    }
    for (int32_t index = 0; index < arraySize; index++) {
        listSub = cJSON_GetArrayItem(ctlCaptureVolList, index);
        if (listSub == NULL) {
            AUDIO_FUNC_LOGE("cJSON_GetArrayItem failed, listSub is NULL!");
            AudioMemFree((void **)&adapter->ctlCaptureNameList);
            return HDF_FAILURE;
        }
        if ((nodeCtlName = cJSON_GetObjectItem(listSub, "name")) == NULL) {
            AudioMemFree((void **)&adapter->ctlCaptureNameList);
            AUDIO_FUNC_LOGE("cJSON_GetObjectItem Failed!");
            return HDF_FAILURE;
        }
        ctlName = nodeCtlName->valuestring;
        ret = strncpy_s(adapter->ctlCaptureNameList[index].name, ALSA_CTL_NAME_LEN - 1, ctlName, strlen(ctlName));
        if (ret != 0) {
            AUDIO_FUNC_LOGE("strncpy_s failed!");
            AudioMemFree((void **)&adapter->ctlCaptureNameList);
            return HDF_FAILURE;
        }
    }
    return HDF_SUCCESS;
}

static int32_t AudioGetVolumeControls(cJSON *object, struct DevProcInfo *adapter)
{
    int32_t ret;
    cJSON *ctlVolumeObj = NULL;
    cJSON *ctlRenderVolumeList = NULL;
    cJSON *ctlCaptureVolumeList = NULL;

    if (object == NULL || adapter == NULL) {
        AUDIO_FUNC_LOGE("Parameter is null");
        return HDF_FAILURE;
    }
    ctlVolumeObj = cJSON_GetObjectItem(object, MIXER_CTL_VOLUME);
    if (ctlVolumeObj == NULL) {
        AUDIO_FUNC_LOGE("cJSON_GetObjectItem failed!");
        return HDF_FAILURE;
    }
    ret = AudioGetVolumeJsonObj(ctlVolumeObj, &ctlRenderVolumeList, PLAYBACK);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("AudioGetVolumeJsonObj failed.");
        return ret;
    }

    ret = AudioGetVolumeJsonObj(ctlVolumeObj, &ctlCaptureVolumeList, CAPTURE);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("AudioGetVolumeJsonObj failed.");
        return ret;
    }

    ret = AudioGetRenderVolumeInstance(adapter, ctlRenderVolumeList);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("AudioGetVolumeInstance failed.");
        return ret;
    }
    ret = AudioGetCaptureVolumeInstance(adapter, ctlCaptureVolumeList);
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("AudioGetVolumeInstance failed.");
        return ret;
    }
    return HDF_SUCCESS;
}

static int32_t AudioAdapterInfoSet(cJSON *adapterObj, struct DevProcInfo *cardDev, enum SndCardType cardType)
{
    int32_t ret;
    struct DevProcInfo *adapter = NULL;

    if (cardDev == NULL || adapterObj == NULL) {
        AUDIO_FUNC_LOGE("Invalid parameter!");
        return HDF_FAILURE;
    }

    adapter = (struct DevProcInfo *)OsalMemCalloc(sizeof(struct DevProcInfo));
    if (adapter == NULL) {
        AUDIO_FUNC_LOGE("calloc cardDev failed!");
        return HDF_FAILURE;
    }

    ret = memcpy_s(adapter->cardName, CARD_ID_LEN_MAX - 1, cardDev->cardName, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter card name fail!");
        AudioMemFree((void **)&adapter);
        return HDF_FAILURE;
    }
    ret = memcpy_s(adapter->cid, CARD_ID_LEN_MAX - 1, cardDev->cid, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter card id fail!");
        AudioMemFree((void **)&adapter);
        return HDF_FAILURE;
    }
    ret = memcpy_s(adapter->did, CARD_ID_LEN_MAX - 1, cardDev->did, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter dai id fail!");
        /* Only log is printed and cannot be returned */
    }

    if (strncmp(cardDev->cardName, PRIMARY, strlen(PRIMARY)) == 0) {
        ret = AudioGetVolumeControls(adapterObj, adapter);
        if (ret < 0) {
            AUDIO_FUNC_LOGE("AudioGetVolumeControls failed!");
            return HDF_FAILURE;
        }
    }

    for (int cardNum = 0; cardNum < AUDIO_MAX_CARD_NUM; cardNum++) {
        if (g_sndCardList[cardType][cardNum] == NULL) {
            g_sndCardList[cardType][cardNum] = adapter;
            break;
        }

        if (cardNum == AUDIO_MAX_CARD_NUM - 1) {
            AUDIO_FUNC_LOGE("The maximum limit for a single type of sound card is %{public}d.", AUDIO_MAX_CARD_NUM);
            AudioMemFree((void **)&adapter);
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static cJSON *AudioGetItemString(cJSON *adapter, char *name)
{
    int32_t ret;
    cJSON *item = NULL;

    if (adapter == NULL || name == NULL) {
        AUDIO_FUNC_LOGE("Invalid parameter!");
        return NULL;
    }

    item = cJSON_GetObjectItem(adapter, name);
    if (item == NULL) {
        AUDIO_FUNC_LOGE("item is null!");
        return NULL;
    }
    if (item->valuestring == NULL) {
        AUDIO_FUNC_LOGE("item valuestring is null!");
        return NULL;
    }

    ret = AudioAdapterCheckName(item->valuestring);
    if (ret < 0) {
        if (strncmp(name, "daiId", sizeof("daiId")) != 0) {
            AUDIO_FUNC_LOGE("The %{public}s name incorrect!", name);
        }
        return NULL;
    }

    return item;
}

static int32_t AudioGetAllItem(cJSON *adapter, struct DevProcInfo *cardDev)
{
    int32_t ret;
    cJSON *adapterName = NULL;
    cJSON *cid = NULL;
    cJSON *did = NULL;

    if (adapter == NULL || cardDev == NULL) {
        AUDIO_FUNC_LOGE("Invalid parameter!");
        return HDF_FAILURE;
    }

    adapterName = AudioGetItemString(adapter, "name");
    if (adapterName == NULL) {
        AUDIO_FUNC_LOGE("Get adapterName failed!");
        return HDF_FAILURE;
    }
    ret = memcpy_s(cardDev->cardName, CARD_ID_LEN_MAX - 1, adapterName->valuestring, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter card name fail!");
        return HDF_FAILURE;
    }

    cid = AudioGetItemString(adapter, "cardId");
    if (cid == NULL) {
        AUDIO_FUNC_LOGE("Get cid failed!");
        return HDF_FAILURE;
    }
    ret = memcpy_s(cardDev->cid, CARD_ID_LEN_MAX - 1, cid->valuestring, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter card id fail!");
        return HDF_FAILURE;
    }

    did = AudioGetItemString(adapter, "daiId");
    if (did == NULL) { // Not all sound cards have dai id.
        return HDF_SUCCESS;
    }
    ret = memcpy_s(cardDev->did, CARD_ID_LEN_MAX - 1, did->valuestring, CARD_ID_LEN_MAX - 1);
    if (ret != EOK) {
        AUDIO_FUNC_LOGE("memcpy_s adapter card id fail!");
        /* Only log is printed and cannot be returned */
    }

    return HDF_SUCCESS;
}

static int32_t AudioParseAdapter(cJSON *adapterObj)
{
    int ret;
    struct DevProcInfo cardDev;
    enum SndCardType cardType = SND_CARD_UNKNOWN;

    if (adapterObj == NULL) {
        AUDIO_FUNC_LOGE("Parameter error!\n");
        return HDF_FAILURE;
    }

    (void)memset_s(&cardDev, sizeof(struct DevProcInfo), 0, sizeof(struct DevProcInfo));
    ret = AudioGetAllItem(adapterObj, &cardDev);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("AudioGetAllItem failed!\n");
        return ret;
    }
    cardType = AudioAdapterNameToType(cardDev.cardName);
    switch (cardType) {
        case SND_CARD_PRIMARY:
        case SND_CARD_HDMI:
        case SND_CARD_USB:
        case SND_CARD_BT:
            ret = AudioAdapterInfoSet(adapterObj, &cardDev, cardType);
            if (ret < 0) {
                AUDIO_FUNC_LOGE("AudioAdapterInfoSet failed!\n");
                return ret;
            }
            break;
        default:
            AUDIO_FUNC_LOGE("Sound card unknown!\n");
            return HDF_FAILURE;
    }

    return HDF_SUCCESS;
}

static int32_t AudioAdaptersSetAdapterVar(cJSON *cJsonObj)
{
    int32_t ret, adaptersArraySize;
    cJSON *adapterObj = NULL;

    if (cJsonObj == NULL) {
        AUDIO_FUNC_LOGE("Parameter error!");
        return HDF_FAILURE;
    }

    adaptersArraySize = cJSON_GetArraySize(cJsonObj);
    if (adaptersArraySize <= 0) {
        AUDIO_FUNC_LOGE("Failed to get JSON array size!");
        return HDF_FAILURE;
    }
    if (adaptersArraySize > MAX_CARD_NUM) {
        AUDIO_FUNC_LOGE("Read adapters number is %{public}d!", adaptersArraySize);
        AUDIO_FUNC_LOGE("The number of sound cards exceeds the upper limit %{public}d.", MAX_CARD_NUM);
        return HDF_FAILURE;
    }

    g_sndCardsNum = adaptersArraySize;
    AUDIO_FUNC_LOGI("sndCardsNum : %{public}d", g_sndCardsNum);

    for (int32_t i = 0; i < adaptersArraySize; i++) {
        adapterObj = cJSON_GetArrayItem(cJsonObj, i);
        if (adapterObj != NULL) {
            ret = AudioParseAdapter(adapterObj);
            if (ret < 0) {
                AUDIO_FUNC_LOGE("AudioParseAdapter (%{public}d) error!", i);
                return HDF_FAILURE;
            }
            adapterObj = NULL;
        }
    }

    return HDF_SUCCESS;
}

int32_t CardInfoParseFromConfig(void)
{
    int32_t ret;
    cJSON *cJsonObj = NULL;
    cJSON *adaptersObj = NULL;

    if (g_parseFlag) {
        return HDF_SUCCESS;
    }

    cJsonObj = AudioCardGetConfig(ALSA_CARD_CONFIG_FILE);
    if (cJsonObj == NULL) {
        AUDIO_FUNC_LOGE("AudioCardGetConfig failed!\n");
        return HDF_FAILURE;
    }

    adaptersObj = cJSON_GetObjectItem(cJsonObj, "adapters");
    if (adaptersObj == NULL) {
        AUDIO_FUNC_LOGE("cJSON_GetObjectItem adapters failed!\n");
        cJSON_Delete(cJsonObj);
        return HDF_FAILURE;
    }

    ret = AudioAdaptersSetAdapterVar(adaptersObj);
    if (ret < 0) {
        AUDIO_FUNC_LOGE("AudioAdaptersSetAdapterVar is failed!\n");
        cJSON_Delete(cJsonObj);
        return HDF_FAILURE;
    }
    cJSON_Delete(cJsonObj);
    g_parseFlag = true;

    return HDF_SUCCESS;
}

static void AudioPortNameFree(struct AudioPort *dataBlock, uint32_t portsLen)
{
    if (dataBlock == NULL) {
        return;
    }

    for (uint32_t i = 0; i < portsLen; i++) {
        OsalMemFree((void *)dataBlock[i].portName);
        dataBlock[i].portName = NULL;
    }
    OsalMemFree(dataBlock);
}

static void AudioFreeDesc(struct AudioAdapterDescriptor **descList, uint32_t sndCardNum)
{
    if (descList == NULL || *descList == NULL) {
        AUDIO_FUNC_LOGE("AudioFreeDesc failed!");
        return;
    }

    for (uint32_t index = 0; index < sndCardNum; index++) {
        if ((*descList)[index].adapterName != NULL) {
            AudioMemFree((void **)&((*descList)[index].adapterName));
            (*descList)[index].adapterName = NULL;
        }
#ifndef AUDIO_HDI_SERVICE_MODE
        AudioPortNameFree((*descList)[index].ports, (*descList)[index].portNum);
#else
        AudioPortNameFree((*descList)[index].ports, (*descList)[index].portsLen);
#endif
    }

    AudioMemFree((void **)descList);
}

static char *AudioMatchCardName(char *cardName, int32_t cardType, int32_t cardNum)
{
    if (strcmp(cardName, PRIMARY) == 0) {
        g_sndCardList[cardType][cardNum]->cardName[0] = 0;
        return strdup("primary");
    } else if (strcmp(cardName, HDMI) == 0) {
        g_sndCardList[cardType][cardNum]->cardName[0] = 0;
        return strdup("hdmi");
    } else {
        AUDIO_FUNC_LOGI("audio card fail to identify");
    }
    return NULL;
}

static char *AudioCardNameTransform(void)
{
    for (int32_t i = 0; i < SND_CARD_MAX; i++) {
        for (int32_t j = 0; j < AUDIO_MAX_CARD_NUM; j++) {
            if (g_sndCardList[i][j] != NULL && strlen(g_sndCardList[i][j]->cardName) != 0) {
                return AudioMatchCardName(g_sndCardList[i][j]->cardName, i, j);
            }
        }
    }
    return NULL;
}

static void InitAudioPortOut(struct AudioPort *audioPort)
{
    audioPort->dir = PORT_OUT;
    audioPort->portId = 0;
    audioPort->portName = strdup("AOP");
}

static void InitAudioPortIn(struct AudioPort *audioPort)
{
    audioPort->dir = PORT_IN;
    audioPort->portId = 0;
    audioPort->portName = strdup("AIP");
}

static void InitAudioPortOutAndIn(struct AudioPort *audioPort)
{
    audioPort->dir = PORT_OUT_IN;
    audioPort->portId = 0;
    audioPort->portName = strdup("AIOP");
}

static int32_t InitAudioPortsForUser(struct AudioAdapterDescriptor *desc)
{
    if (desc == NULL || desc->adapterName == NULL || desc->ports == NULL) {
        AUDIO_FUNC_LOGE("Paras is null!");
        return HDF_ERR_INVALID_PARAM;
    }

    if (strcmp(desc->adapterName, PRIMARY) == 0) {
        InitAudioPortOut(&desc->ports[0]);
        InitAudioPortIn(&desc->ports[SUPPORT_CAPTURE_OR_RENDER]);
        InitAudioPortOutAndIn(&desc->ports[SUPPORT_CAPTURE_AND_RENDER]);
    } else if (strcmp(desc->adapterName, HDMI) == 0) {
        InitAudioPortOut(&desc->ports[0]);
    } else if (strcmp(desc->adapterName, USB) == 0) {
        InitAudioPortOut(&desc->ports[0]);
        InitAudioPortIn(&desc->ports[SUPPORT_CAPTURE_OR_RENDER]);
    } else {
        AUDIO_FUNC_LOGE("Unknown sound card type does not support this sound card temporarily!");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t AudioReadCardPortToDesc(struct AudioAdapterDescriptor *desc)
{
    uint8_t portNum;

    if (desc == NULL || desc->adapterName == NULL) {
        AUDIO_FUNC_LOGE("descs or desc->adapterName is NULL!");
        return HDF_ERR_INVALID_PARAM;
    }

    if (strcmp(desc->adapterName, PRIMARY) == 0) {
        portNum = PORT_OUT_IN; /* support (capture, render, capture & render) */
    } else if (strcmp(desc->adapterName, HDMI) == 0) {
        portNum = PORT_OUT; /* support render */
    } else if (strcmp(desc->adapterName, USB) == 0) {
        portNum = PORT_IN; /* support (capture, render) */
    } else {
        AUDIO_FUNC_LOGE("Unknown sound card type does not support this sound card temporarily!");
        return HDF_FAILURE;
    }

#ifndef AUDIO_HDI_SERVICE_MODE
    desc->portNum = portNum;
#else
    desc->portsLen = portNum;
#endif

    desc->ports = (struct AudioPort *)OsalMemCalloc(sizeof(struct AudioPort) * portNum);
    if (desc->ports == NULL) {
        AUDIO_FUNC_LOGE("OsalMemCalloc failed!");
        return HDF_FAILURE;
    }
    if (InitAudioPortsForUser(desc) < 0) {
        AUDIO_FUNC_LOGE("InitAudioPortsForUser failed!");
        AudioMemFree((void **)&desc->ports);
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t AudioReadCardInfoToDesc(struct AudioAdapterDescriptor **descs, uint8_t cardNum, bool usbAudioState)
{
    int32_t index = 0;
    int32_t cardNumTemp = 0;
    if (descs == NULL || cardNum == 0) {
        AUDIO_FUNC_LOGE("Parameter error.");
        return HDF_FAILURE;
    }
    cardNumTemp = cardNum;
    if (*descs == NULL) {
        AUDIO_FUNC_LOGW("*descs is null, need memcalloc.");
        *descs = (struct AudioAdapterDescriptor *)OsalMemCalloc(sizeof(struct AudioAdapterDescriptor) * cardNum);
        if (*descs == NULL) {
            AUDIO_FUNC_LOGE("OsalMemCalloc descs is NULL");
            return HDF_FAILURE;
        }
    }
    if (usbAudioState) {
        (*descs)[cardNum - 1].adapterName = strdup(USB);
        cardNumTemp = cardNum - 1;
    }

    for (index = 0; index < cardNumTemp; index++) {
        (*descs)[index].adapterName = AudioCardNameTransform();
        if ((*descs)[index].adapterName == NULL) {
            AUDIO_FUNC_LOGE("(*descs)[index].adapterName is NULL!");
            AudioFreeDesc(descs, cardNum);
            return HDF_FAILURE;
        }
    }

    for (index = 0; index < cardNum; index++) {
        if (AudioReadCardPortToDesc(&(*descs)[index]) != HDF_SUCCESS) {
            AUDIO_FUNC_LOGE("read port failed!");
            AudioFreeDesc(descs, cardNum);
            return HDF_FAILURE;
        }
    }

    return HDF_SUCCESS;
}

static bool CheckUsbAudioIsInserted(struct AlsaCardsList *cardList)
{
    if (cardList == NULL) {
        AUDIO_FUNC_LOGE("The parameter is empty.");
        return false;
    }

    for (int32_t i = 0; i < MAX_CARD_NUM; i++) {
        /** External codec */
        if (strcmp(USB_AUDIO, cardList->alsaDevIns[i].pcmInfoId) == 0) {
            return true;
        }
    }

    return false;
}

int32_t AudioGetAllCardInfo(struct AudioAdapterDescriptor **descs, int32_t *sndCardNum)
{
    if (descs == NULL || sndCardNum == NULL) {
        AUDIO_FUNC_LOGE("descs or sndCardNum is NULL!");
        return HDF_ERR_INVALID_PARAM;
    }
    AUDIO_FUNC_LOGI("enter!");
    /* Parse sound card from configuration file */
    int32_t ret = CardInfoParseFromConfig();
    if (ret != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("parse config file failed! ret = %{public}d", ret);
        return HDF_FAILURE;
    }

    /* Read sound card list from alsa hardware */
    (void)memset_s(&g_alsaCardsList, sizeof(struct AlsaCardsList), 0, sizeof(struct AlsaCardsList));
    GetDeviceList(&g_alsaCardsList, SND_PCM_STREAM_PLAYBACK);

    /* Check whether there is a USB sound card through pcmInfoId ("USB Audio") */
    bool usbAduioFlag = CheckUsbAudioIsInserted(&g_alsaCardsList);
    if (usbAduioFlag) {
        g_sndCardsNum += 1;
    }

    *sndCardNum = g_sndCardsNum;
    AUDIO_FUNC_LOGI("CardNum : %{public}d\n", *sndCardNum);

    /* Reconstruct sound card information to user */
    if (AudioReadCardInfoToDesc(descs, g_sndCardsNum, usbAduioFlag) != HDF_SUCCESS) {
        AUDIO_FUNC_LOGE("AudioReadCardInfoToDesc failed!");
        *sndCardNum = 0;
        CardInfoRelease();
        return HDF_FAILURE;
    }

    CardInfoRelease();

    AUDIO_FUNC_LOGI("AudioGetAllCardInfo succ!");

    return HDF_SUCCESS;
}
