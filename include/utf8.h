#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <string.h>

/*
 * 将 src 复制到容量为 cap 的固定缓冲区 dst，保证三点：
 *   1) 不溢出；
 *   2) 以 NUL 结尾；
 *   3) 不在多字节 UTF-8 字符的中间截断。
 *
 * 第 3 点是关键：byte 级截断（如 strncpy(dst, src, cap-1)）会留下残缺的
 * UTF-8 序列（孤立的引导字节）。一旦该字符串作为 WebSocket 文本帧广播，
 * 而 RFC 6455 规定文本帧必须是合法 UTF-8，接收方客户端必须断开连接——
 * 表现为“整房间客户端被踢出”。当某个字段首次超出固定缓冲长度时即触发，
 * 例如含 16 位歌手、UTF-8 字节数超过 128 的歌名。
 *
 * 放不下时回退到最近的字符边界，整体丢弃末尾那个不完整的字符。
 * 假定输入是合法 UTF-8（来自 cJSON 解析的 JSON 字符串即满足）。
 */
static inline void copy_utf8_bounded(char *dst, const char *src, size_t cap)
{
    if (cap == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    size_t i = 0;
    size_t end = 0; /* 已确认完整字符的结束位置 */
    while (src[i])
    {
        unsigned char c = (unsigned char)src[i];
        size_t char_len;
        if (c < 0x80)
            char_len = 1;
        else if ((c & 0xE0) == 0xC0)
            char_len = 2;
        else if ((c & 0xF0) == 0xE0)
            char_len = 3;
        else if ((c & 0xF8) == 0xF0)
            char_len = 4;
        else
            char_len = 1; /* 非法首字节按单字节处理（合法输入不会出现） */

        /* 整个字符放不下，停止——避免留下残缺字符 */
        if (i + char_len > cap - 1)
            break;

        i += char_len;
        end = i;
    }

    memcpy(dst, src, end);
    dst[end] = '\0';
}

#endif /* UTF8_H */
