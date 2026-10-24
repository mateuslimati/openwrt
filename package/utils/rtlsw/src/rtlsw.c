/*
 * rtlsw - controle do switch Realtek RTL8365MB (API rtl8367b) da LS1021A-IoT
 * pelo DSPI1 (0x02100000), chip select 1.
 *
 * Dois backends:
 *   spidev   - kernel com CONFIG_SPI_FSL_DSPI e o DSPI1 exposto como spidev
 *              (procura /sys/class/spidev/<x> cujo controlador e 2100000.spi;
 *              RTLSW_DEV=/dev/spidevX.Y forca um dispositivo)
 *   /dev/mem - kernel sem driver DSPI (ex.: SDK 3.12), programando o DSPI direto
 *
 * Protocolo (engenharia reversa de /usr/sbin/rtlinit):
 *   escrita: 0x02, A[15:8], A[7:0], D[15:8], D[7:0]   (CS=PCS1, CONT ate o ultimo byte)
 *   leitura: 0x03, A[15:8], A[7:0], dummy,  dummy  -> D[15:8], D[7:0]
 *
 * Os registradores do DSPI no LS1021A sao big-endian.
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -static -o rtlsw rtlsw.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/spi/spidev.h>
#include <time.h>

#define DSPI1   0x02100000
#define MCR     0x00
#define CTAR0   0x0C
#define SR      0x2C
#define PUSHR   0x34
#define POPR    0x38
#define SR_TCF  (1u << 31)
#define SR_RFDF (1u << 17)
#define CONT    (1u << 31)
#define PCS1    0x00020000u

static volatile uint32_t *dspi;
static int spifd = -1;

static uint32_t rd32(int o) { return __builtin_bswap32(dspi[o / 4]); }
static void wr32(int o, uint32_t v) { dspi[o / 4] = __builtin_bswap32(v); }

/* spidev ligado ao controlador 2100000.spi, chip select 1 */
static int spidev_find(char *dev, size_t len)
{
	const char *env = getenv("RTLSW_DEV");
	glob_t g;
	int found = 0;

	if (env) {
		snprintf(dev, len, "%s", env);
		return 1;
	}
	if (glob("/sys/class/spidev/spidev*.1", 0, NULL, &g))
		return 0;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		char lnk[PATH_MAX], real[PATH_MAX];
		snprintf(lnk, sizeof(lnk), "%s/device", g.gl_pathv[i]);
		if (realpath(lnk, real) && strstr(real, "2100000.spi")) {
			snprintf(dev, len, "/dev/%s", strrchr(g.gl_pathv[i], '/') + 1);
			found = 1;
		}
	}
	globfree(&g);
	return found;
}

static void spi_init(void)
{
	char dev[64];

	if (spidev_find(dev, sizeof(dev))) {
		uint8_t mode = SPI_MODE_0, bits = 8;
		uint32_t hz = 1000000;
		spifd = open(dev, O_RDWR);
		if (spifd < 0) { perror(dev); exit(1); }
		ioctl(spifd, SPI_IOC_WR_MODE, &mode);
		ioctl(spifd, SPI_IOC_WR_BITS_PER_WORD, &bits);
		ioctl(spifd, SPI_IOC_WR_MAX_SPEED_HZ, &hz);
		return;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "sem spidev do DSPI1 e sem /dev/mem: habilite CONFIG_SPI_FSL_DSPI + spidev no DT\n");
		exit(1);
	}
	dspi = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DSPI1);
	if (dspi == MAP_FAILED) { perror("mmap"); exit(1); }
	/* mesma configuracao do rtlinit: master, PCS1 ativo-baixo, FIFOs limpos */
	wr32(SR, 0xffffffff);
	wr32(MCR, 0x80020C00);
	wr32(CTAR0, 0x38B00007);	/* 8 bits, CPOL=0 CPHA=0, PBR=/2 BR=/128 */
}

static void xfer(const uint8_t *tx, uint8_t *rx, int n)
{
	if (spifd >= 0) {
		struct spi_ioc_transfer t = {
			.tx_buf = (uintptr_t)tx, .rx_buf = (uintptr_t)rx, .len = n,
		};
		if (ioctl(spifd, SPI_IOC_MESSAGE(1), &t) < 0) {
			perror("SPI_IOC_MESSAGE");
			exit(2);
		}
		return;
	}
	for (int i = 0; i < n; i++) {
		int spin = 0;
		wr32(SR, SR_RFDF | SR_TCF);
		wr32(PUSHR, (i == n - 1 ? 0 : CONT) | PCS1 | tx[i]);
		while (!(rd32(SR) & SR_RFDF))
			if (++spin > 1000000) {
				fprintf(stderr, "DSPI timeout SR=0x%08x\n", rd32(SR));
				exit(2);
			}
		rx[i] = rd32(POPR) & 0xff;
	}
}

static uint16_t rd(uint16_t a)
{
	uint8_t tx[5] = { 0x03, a >> 8, a & 0xff, 0, 0 }, rx[5];
	xfer(tx, rx, 5);
	return (rx[3] << 8) | rx[4];
}

static void wr(uint16_t a, uint16_t v)
{
	uint8_t tx[5] = { 0x02, a >> 8, a & 0xff, v >> 8, v & 0xff }, rx[5];
	xfer(tx, rx, 5);
}

static int phy_wait(void)
{
	for (int i = 0; i < 1000; i++)
		if (!(rd(0x1f01) & 4))
			return 0;
	fprintf(stderr, "phy busy\n");
	return -1;
}

static uint16_t phy_rd(int p, int r)
{
	wr(0x1f02, 0x2000 + 32 * p + r);
	wr(0x1f00, 0x1);
	phy_wait();
	return rd(0x1f04);
}

static void phy_wr(int p, int r, uint16_t v)
{
	wr(0x1f03, v);
	wr(0x1f02, 0x2000 + 32 * p + r);
	wr(0x1f00, 0x3);
	phy_wait();
}

/* contador MIB (offset em palavras de 16 bits, como no rtl8367b.c do OpenWrt):
 * o ASIC prepara uma janela de 64 bits (regs 0x1000..0x1003) alinhada em 4 palavras */
static int mib(int port, int off, int len, uint64_t *val)
{
	uint16_t c = 0;
	wr(0x1004, (port * 0x7c + off) >> 2);
	for (int i = 0; i < 1000; i++)
		if (!((c = rd(0x1005)) & 1))
			break;
	if (c & 2)
		return -1;
	int top = len == 4 ? 3 : (off + 1) % 4;
	*val = 0;
	for (int i = 0; i < len; i++)
		*val = (*val << 16) | rd(0x1000 + top - i);
	return 0;
}

static const char *pn[] = { "P0", "P1", "P2", "P3", "P4", "EXT0", "EXT1", "EXT2" };
static const char *spd[] = { "10M", "100M", "1000M", "?" };

static const struct { const char *n; int off, len; } mibs[] = {
	{ "ifInOctets", 0, 4 },
	{ "dot3StatsFCSErrors", 4, 2 },
	{ "dot3StatsSymbolErrors", 6, 2 },
	{ "dot3InPauseFrames", 8, 2 },
	{ "dot3ControlInUnknownOpcodes", 10, 2 },
	{ "etherStatsFragments", 12, 2 },
	{ "etherStatsJabbers", 14, 2 },
	{ "ifInUcastPkts", 16, 2 },
	{ "etherStatsDropEvents", 18, 2 },
	{ "ifInMulticastPkts", 20, 2 },
	{ "ifInBroadcastPkts", 22, 2 },
	{ "inMldChecksumError", 24, 2 },
	{ "inIgmpChecksumError", 26, 2 },
	{ "inMldSpecificQuery", 28, 2 },
	{ "inMldGeneralQuery", 30, 2 },
	{ "inIgmpSpecificQuery", 32, 2 },
	{ "inIgmpGeneralQuery", 34, 2 },
	{ "inMldLeaves", 36, 2 },
	{ "inIgmpLeaves", 38, 2 },
	{ "etherStatsOctets", 40, 4 },
	{ "etherStatsUnderSizePkts", 44, 2 },
	{ "etherOversizeStats", 46, 2 },
	{ "etherStatsPkts64Octets", 48, 2 },
	{ "etherStatsPkts65to127Octets", 50, 2 },
	{ "etherStatsPkts128to255Octets", 52, 2 },
	{ "etherStatsPkts256to511Octets", 54, 2 },
	{ "etherStatsPkts512to1023Octets", 56, 2 },
	{ "etherStatsPkts1024to1518Octets", 58, 2 },
	{ "ifOutOctets", 60, 4 },
	{ "dot3StatsSingleCollisionFrames", 64, 2 },
	{ "dot3StatMultipleCollisionFrames", 66, 2 },
	{ "dot3sDeferredTransmissions", 68, 2 },
	{ "dot3StatsLateCollisions", 70, 2 },
	{ "etherStatsCollisions", 72, 2 },
	{ "dot3StatsExcessiveCollisions", 74, 2 },
	{ "dot3OutPauseFrames", 76, 2 },
	{ "ifOutDiscards", 78, 2 },
	{ "dot1dTpPortInDiscards", 80, 2 },
	{ "ifOutUcastPkts", 82, 2 },
	{ "ifOutMulticastPkts", 84, 2 },
	{ "ifOutBroadcastPkts", 86, 2 },
	{ "outOampduPkts", 88, 2 },
	{ "inOampduPkts", 90, 2 },
	{ "inIgmpJoinsSuccess", 92, 2 },
	{ "inIgmpJoinsFail", 94, 2 },
	{ "inMldJoinsSuccess", 96, 2 },
	{ "inMldJoinsFail", 98, 2 },
	{ "inReportSuppressionDrop", 100, 2 },
	{ "inLeaveSuppressionDrop", 102, 2 },
	{ "outIgmpReports", 104, 2 },
	{ "outIgmpLeaves", 106, 2 },
	{ "outIgmpGeneralQuery", 108, 2 },
	{ "outIgmpSpecificQuery", 110, 2 },
	{ "outMldReports", 112, 2 },
	{ "outMldLeaves", 114, 2 },
	{ "outMldGeneralQuery", 116, 2 },
	{ "outMldSpecificQuery", 118, 2 },
	{ "inKnownMulticastPkts", 120, 2 },
};

static void usage(void)
{
	puts("rtlsw - RTL8365MB via DSPI1 (LS1021A-IoT)\n"
	     "  rtlsw init                  liga a EXT1 (CPU) em RGMII 1000/FD\n"
	     "  rtlsw id                    identifica o chip\n"
	     "  rtlsw rd <reg>              le registrador\n"
	     "  rtlsw wr <reg> <val>        escreve registrador\n"
	     "  rtlsw dump <ini> <fim>      despeja faixa\n"
	     "  rtlsw ports                 status de link das portas\n"
	     "  rtlsw ext                   interfaces externas (RGMII/MII)\n"
	     "  rtlsw phy <n> [reg [val]]   PHY interna n (0-4)\n"
	     "  rtlsw mib <porta>           contadores MIB\n"
	     "  rtlsw mibreset              zera todos os contadores\n"
	     "  rtlsw isol [porta mask]     isolamento de portas\n"
	     "  rtlsw vlan                  tabela VLAN MC + PVID\n"
	     "  rtlsw bench                 mede taxa de acesso SPI\n"
	     "  rtlsw peek <addr> [n]       le registrador(es) BE do SoC");
}

#define H(s) ((int)strtol((s), 0, 0))

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 0; }
	if (!strcmp(argv[1], "peek") && argc > 2) {
		/* leitura 32 bits de registrador do SoC (CCSR big-endian) */
		unsigned long a = strtoul(argv[2], 0, 0);
		int n = argc > 3 ? H(argv[3]) : 1;
		int fd = open("/dev/mem", O_RDONLY | O_SYNC);
		volatile uint32_t *p = mmap(0, 0x2000, PROT_READ, MAP_SHARED, fd, a & ~0xfffUL);
		if (fd < 0 || p == MAP_FAILED) { perror("peek"); return 1; }
		for (int i = 0; i < n; i++)
			printf("0x%08lx = 0x%08x\n", a + 4 * i,
			       __builtin_bswap32(p[((a & 0xfff) >> 2) + i]));
		return 0;
	}
	spi_init();
	const char *c = argv[1];

	if (!strcmp(c, "init")) {
		/* minimo para a CPU (eTSEC3/eth2) falar com o switch apos reset */
		wr(0x13c2, 0x0249);	/* magic de desbloqueio */
		wr(0x1305, 0x0010);	/* EXT1 = RGMII */
		wr(0x1311, 0x1016);	/* EXT1 forcada: link, 1000M, full duplex */
		wr(0x1307, 0x000c);	/* EXT1 txdly=1 (2 ns), rxdly=4 */
		uint16_t f = rd(0x1311);
		printf("EXT1 force=0x%04x %s\n", f, f == 0x1016 ? "ok" : "FALHOU");
		return f == 0x1016 ? 0 : 1;
	} else if (!strcmp(c, "id")) {
		wr(0x13c2, 0x0249);	/* magic de desbloqueio */
		uint16_t n = rd(0x1300), v = rd(0x1301), m = rd(0x1302);
		printf("chip number 0x%04x  ver 0x%04x (RLVID=%x MCID=%x BOID=%x AFE=%x)  mode 0x%04x\n",
		       n, v, v >> 12, (v >> 8) & 15, (v >> 4) & 15, v & 1, m);
	} else if (!strcmp(c, "rd") && argc > 2) {
		int r = H(argv[2]);
		printf("0x%04x = 0x%04x\n", r, rd(r));
	} else if (!strcmp(c, "wr") && argc > 3) {
		int r = H(argv[2]), v = H(argv[3]);
		wr(r, v);
		printf("0x%04x <- 0x%04x (le: 0x%04x)\n", r, v, rd(r));
	} else if (!strcmp(c, "dump") && argc > 3) {
		int lo = H(argv[2]) & ~7, hi = H(argv[3]);
		for (int r = lo; r <= hi; r += 8) {
			printf("%04x:", r);
			for (int i = 0; i < 8; i++)
				printf(" %04x", rd(r + i));
			putchar('\n');
		}
	} else if (!strcmp(c, "ports")) {
		for (int p = 0; p < 8; p++) {
			uint16_t s = rd(0x1352 + p);
			printf("%-5s 0x%04x  %-4s %-5s %s%s%s%s%s\n", pn[p], s,
			       s & 0x10 ? "UP" : "down", s & 0x10 ? spd[s & 3] : "-",
			       s & 4 ? "FD" : "HD", s & 0x80 ? " nway" : "",
			       s & 0x40 ? " txpause" : "", s & 0x20 ? " rxpause" : "",
			       s & 0x100 ? " master" : "");
		}
	} else if (!strcmp(c, "ext")) {
		static const char *mo[] = { "disable", "RGMII", "MII-MAC", "MII-PHY", "TMII-MAC",
					    "TMII-PHY", "GMII", "RMII-MAC", "RMII-PHY" };
		uint16_t dis = rd(0x1305), dis2 = rd(0x13c3);
		int m[3] = { dis & 0xf, (dis >> 4) & 0xf, dis2 & 0xf };
		int fr[3] = { 0x1310, 0x1311, 0x13c4 }, dr[3] = { 0x1306, 0x1307, 0x13c5 };
		printf("DIS(0x1305)=0x%04x DIS2(0x13c3)=0x%04x\n", dis, dis2);
		for (int x = 0; x < 3; x++) {
			uint16_t f = rd(fr[x]), d = rd(dr[x]);
			printf("EXT%d mode=%-8s force=0x%04x (forcemode=%d link=%d %s %s%s%s) rgmii txdly=%d rxdly=%d\n",
			       x, m[x] < 9 ? mo[m[x]] : "?", f, (f >> 12) & 1, (f >> 4) & 1,
			       spd[f & 3], f & 4 ? "FD" : "HD", f & 0x40 ? " txp" : "",
			       f & 0x20 ? " rxp" : "", (d >> 3) & 1, d & 7);
		}
	} else if (!strcmp(c, "phy") && argc > 2) {
		int p = H(argv[2]);
		if (argc == 3)
			for (int r = 0; r < 16; r++)
				printf("phy%d reg%02d = 0x%04x\n", p, r, phy_rd(p, r));
		else if (argc == 4)
			printf("0x%04x\n", phy_rd(p, H(argv[3])));
		else
			phy_wr(p, H(argv[3]), H(argv[4]));
	} else if (!strcmp(c, "mib") && argc > 2) {
		int p = H(argv[2]);
		for (unsigned i = 0; i < sizeof(mibs) / sizeof(mibs[0]); i++) {
			uint64_t v;
			if (mib(p, mibs[i].off, mibs[i].len, &v) < 0) { printf("%-30s (reset)\n", mibs[i].n); continue; }
			printf("%-30s %llu\n", mibs[i].n, (unsigned long long)v);
		}
	} else if (!strcmp(c, "mibreset")) {
		wr(0x1005, 1 << 11);
	} else if (!strcmp(c, "isol")) {
		if (argc > 3)
			wr(0x08a2 + H(argv[2]), H(argv[3]));
		for (int p = 0; p < 8; p++) {
			uint16_t m = rd(0x08a2 + p);
			printf("%-5s -> 0x%02x [", pn[p], m);
			for (int i = 0; i < 8; i++)
				if (m & (1 << i))
					printf(" %s", pn[i]);
			puts(" ]");
		}
	} else if (!strcmp(c, "vlan")) {
		uint16_t vc = rd(0x07a8);
		printf("VLAN_CTRL(0x07a8)=0x%04x  802.1Q %s\n", vc, vc & 1 ? "ON" : "OFF");
		for (int p = 0; p < 8; p++) {
			uint16_t v = rd(0x0700 + p / 2);
			printf("%-5s PVID -> MC%d\n", pn[p], (v >> (8 * (p % 2))) & 0x1f);
		}
		for (int i = 0; i < 32; i++) {
			uint16_t w[4];
			for (int k = 0; k < 4; k++)
				w[k] = rd(0x0728 + i * 4 + k);
			if (w[0] || w[3])
				printf("MC%02d member=0x%02x fid=%d evid=%d\n", i, w[0] & 0xff, w[1] & 0xf, w[3] & 0x1fff);
		}
	} else if (!strcmp(c, "bench")) {
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (int i = 0; i < 1000; i++)
			rd(0x1300);
		clock_gettime(CLOCK_MONOTONIC, &b);
		double us = (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
		printf("1000 leituras em %.0f us (%.1f us/leitura)\n", us, us / 1000);
	} else {
		usage();
	}
	return 0;
}
