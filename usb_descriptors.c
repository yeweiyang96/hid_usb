#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

// 保持你原有的罗技优联接收器硬件指纹
#define USB_PID   0xC52C
#define USB_VID   0x046D
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// Device Descriptors (保持原样)
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// HID Report Descriptor (拆分为独立的键盘和鼠标描述符)
//--------------------------------------------------------------------+
uint8_t const desc_hid_keyboard_report[] =
{
  TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(REPORT_ID_KEYBOARD) )
};

uint8_t const desc_hid_mouse_report[] =
{
  TUD_HID_REPORT_DESC_MOUSE( HID_REPORT_ID(REPORT_ID_MOUSE) )
};

// 根据实例返回对应的报告描述符
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
  if (instance == 0) return desc_hid_keyboard_report;
  return desc_hid_mouse_report;
}

//--------------------------------------------------------------------+
// Configuration Descriptor (修正为双独立标准接口协议)
//--------------------------------------------------------------------+
enum
{
  ITF_NUM_HID_KEYBOARD,  // 接口 0：标准键盘
  ITF_NUM_HID_MOUSE,     // 接口 1：标准鼠标
  ITF_NUM_TOTAL
};

// 总长度计算：1个配置描述符长度 + 2个HID描述符长度
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + (2 * TUD_HID_DESC_LEN))

// 声明两个独立的硬件端点 (Endpoint In)
#define EPNUM_HID_KEYBOARD   0x81
#define EPNUM_HID_MOUSE      0x82

uint8_t const desc_configuration[] =
{
  // 配置头
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

  // 接口 0：完美的硬件标准键盘协议声明 (HID_ITF_PROTOCOL_KEYBOARD)
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_keyboard_report), EPNUM_HID_KEYBOARD, CFG_TUD_HID_EP_BUFSIZE, 8),

  // 接口 1：完美的硬件标准鼠标协议声明 (HID_ITF_PROTOCOL_MOUSE)
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_mouse_report), EPNUM_HID_MOUSE, CFG_TUD_HID_EP_BUFSIZE, 8)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
};

char const *string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: 语言 ID
  "Logitech",                    // 1: 厂商字符串
  "Unifying Receiver",           // 2: 产品字符串
  "203A4C8F15BD"                 // 3: 序列号
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  switch ( index ) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    default:
      if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;

      const char *str = string_desc_arr[index];
      chr_count = strlen(str);
      size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; 
      if ( chr_count > max_count ) chr_count = max_count;

      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = str[i];
      }
      break;
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}