/*
 * serial_tool - Interactive UART debugging utility
 *
 * Author: Ted Falasco
 *
 * Portions of this code were developed with assistance from ChatGPT
 * (OpenAI) as a programming aid. All code was reviewed and tested by
 * the author.
 * 
 * Version 1.0
 * Date: 2026-03-13
 */

// These two lines not entirely required, but needed for Intelisense bug
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>

#define RX_BUF_SIZE 256
#define LINE_BUF_SIZE 256
#define PACKET_TIMEOUT_MS 30
#define PACKET_BUF_SIZE 1024

static speed_t baud_to_flag(int baud)
{
    switch (baud)
    {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default:
            fprintf(stderr,"Unsupported baud\n");
            exit(1);
    }
}

static int serial_open(const char *device, int baud)
{
    int fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);

    if (fd < 0)
    {
        perror("open");
        exit(1);
    }

    struct termios tty;

    if (tcgetattr(fd, &tty) != 0)
    {
        perror("tcgetattr");
        exit(1);
    }

    speed_t speed = baud_to_flag(baud);

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;

    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        perror("tcsetattr");
        exit(1);
    }

    return fd;
}

static void timestamp()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm *tm = localtime(&ts.tv_sec);

    printf("%02d:%02d:%02d.%03ld ",
           tm->tm_hour,
           tm->tm_min,
           tm->tm_sec,
           ts.tv_nsec / 1000000);
}

static void print_hex_ascii(uint8_t *buf, int len)
{
    printf("RX %3d : ", len);

    for (int i=0;i<len;i++)
        printf("%02X ", buf[i]);

    printf(" |");

    for (int i=0;i<len;i++)
    {
        if (buf[i] >= 32 && buf[i] <= 126)
            printf("%c", buf[i]);
        else
            printf(".");
    }

    printf("|\n");
}

static int parse_hex_line(char *line, uint8_t *buf)
{
    int count = 0;

    /* allow commas and whitespace as separators */
    char *token = strtok(line, " ,\t\r\n");

    while (token && count < 256)
    {
        /* skip optional 0x prefix */
        if ((token[0] == '0') && (token[1] == 'x' || token[1] == 'X'))
            token += 2;

        char *end;
        long val = strtol(token, &end, 16);

        if (end != token && val >= 0 && val <= 0xFF)
        {
            buf[count++] = (uint8_t)val;
        }
        else
        {
            printf("Ignoring invalid byte: %s\n", token);
        }

        token = strtok(NULL, " ,\t\r\n");
    }

    return count;
}

static int parse_ascii_line(char *line, uint8_t *buf)
{
    char *start = strchr(line, '"');
    char *end   = strrchr(line, '"');

    if (!start || !end || start == end)
        return 0;

    start++; // skip opening quote

    int len = end - start;

    for (int i = 0; i < len; i++)
        buf[i] = (uint8_t)start[i];

    return len;
}

static void print_tx(uint8_t *buf, int len)
{
    printf("TX %3d : ", len);

    for (int i=0;i<len;i++)
        printf("%02X ", buf[i]);

    printf("\n");
}

static const char *progname(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static long time_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("Usage:\n");
        printf("  %s <device> <baud>\n", progname(argv[0]));
        return 0;
    }

    const char *device = argv[1];
    int baud = atoi(argv[2]);

    int fd = serial_open(device, baud);

    printf("Opened %s @ %d\n", device, baud);
    printf("Enter hex (AA 01 02), comma (AA,01), or ASCII \"text\"\n");
    printf("> ");

    uint8_t rxbuf[RX_BUF_SIZE];
    uint8_t txbuf[256];
    char line[LINE_BUF_SIZE];
    uint8_t packet_buf[PACKET_BUF_SIZE];
    int packet_len = 0;
    long last_rx_time = time_ms();

    while (1)
    {
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000 * PACKET_TIMEOUT_MS;

        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0)
        {
            perror("select");
            break;
        }

        /* ---- INSERT FLUSH CHECK HERE ---- */

        long now = time_ms();

        if (packet_len > 0 && (now - last_rx_time) > PACKET_TIMEOUT_MS)
        {
            timestamp();
            printf("Packet (%d):\n", packet_len);
            print_hex_ascii(packet_buf, packet_len);
            printf("\n");

            packet_len = 0;

            printf("> ");
            fflush(stdout);
        }

        /* ---- SERIAL INPUT ---- */

        if (FD_ISSET(fd, &readfds))
        {
            int n = read(fd, rxbuf, RX_BUF_SIZE);

            if (n > 0)
            {
                now = time_ms();

                /* detect boundary between packets */

                if (packet_len > 0 && (now - last_rx_time) > PACKET_TIMEOUT_MS)
                {
                    timestamp();
                    printf("Packet (%d):\n", packet_len);
                    print_hex_ascii(packet_buf, packet_len);
                    printf("\n");

                    packet_len = 0;
                }

                if (packet_len + n < PACKET_BUF_SIZE)
                {
                    memcpy(&packet_buf[packet_len], rxbuf, n);
                    packet_len += n;
                }

                last_rx_time = now;
            }
        }

        /* ---- USER INPUT ---- */

        if (FD_ISSET(STDIN_FILENO, &readfds))
        {
            if (fgets(line, sizeof(line), stdin))
            {
                int len;

                if (line[0] == '"')
                    len = parse_ascii_line(line, txbuf);
                else
                    len = parse_hex_line(line, txbuf);

                if (len > 0)
                {
                    write(fd, txbuf, len);
                    print_tx(txbuf, len);
                }

                printf("> ");
                fflush(stdout);
            }
        }
    }

    close(fd);
}