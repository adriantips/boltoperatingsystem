#include <stdint.h>
#include "commands.h"
#include "hw.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  Networking commands.  BoltOS has no NIC driver and no TCP/IP stack, so the
 *  transport commands report honestly instead of pretending. What IS real:
 *  probing PCI for a network controller, and the in-RAM firewall ruleset.
 * ===========================================================================*/

static int find_nic(struct pci_dev *out) {
    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    for (int i = 0; i < n; i++)
        if (pds[i].class == 0x02 || pds[i].class == 0x0D) { *out = pds[i]; return 1; }
    return 0;
}
static void no_net(const char *cmd) {
    kprintf("%s: no network interface (no NIC driver / TCP-IP stack)\n", cmd);
}

int cmd_netinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    struct pci_dev nic;
    kprintf("interfaces: none configured\n");
    kprintf("stack     : not implemented (no TCP/IP)\n");
    if (find_nic(&nic))
        kprintf("hardware  : PCI %x:%x.%x  %x:%x  (%s) - no driver bound\n",
                nic.bus, nic.slot, nic.func, nic.vendor, nic.device,
                pci_class_name(nic.class));
    else
        kprintf("hardware  : no PCI network controller present\n");
    return 0;
}

int cmd_ping(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: ping HOST\n"); return 1; }
    kprintf("ping %s: ", argv[1]);
    no_net("ping");
    return 1;
}

int cmd_trace(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: trace HOST\n"); return 1; }
    kprintf("trace to %s: ", argv[1]);
    no_net("trace");
    return 1;
}

int cmd_download(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: download URL\n"); return 1; }
    no_net("download");
    return 1;
}

int cmd_upload(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: upload FILE\n"); return 1; }
    no_net("upload");
    return 1;
}

int cmd_share(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: share PATH\n"); return 1; }
    no_net("share");
    return 1;
}

int cmd_wifi(int argc, char **argv) {
    (void)argc; (void)argv;
    struct pci_dev nic;
    if (find_nic(&nic) && nic.class == 0x0D)
        kprintf("wifi: wireless controller %x:%x present but no driver\n",
                nic.vendor, nic.device);
    else
        kprintf("wifi: no wireless adapter detected\n");
    return 0;
}

/* ports: no TCP/UDP listeners (no stack). Show the real hardware I/O port map
 * instead, which is the closest thing BoltOS actually has. */
int cmd_ports(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("network ports: none (no TCP/UDP stack)\n");
    kprintf("hardware I/O port map:\n");
    kprintf("  0x0020-0x0021  PIC master\n");
    kprintf("  0x0040-0x0043  PIT timer\n");
    kprintf("  0x0060,0x0064  PS/2 keyboard\n");
    kprintf("  0x0070-0x0071  CMOS/RTC\n");
    kprintf("  0x00A0-0x00A1  PIC slave\n");
    kprintf("  0x03F8-0x03FF  COM1 serial\n");
    kprintf("  0x0CF8-0x0CFF  PCI config\n");
    return 0;
}

/* scan: discover devices. No network, so this enumerates the PCI bus - the
 * real devices attached to the machine. */
int cmd_scan(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no network to scan; enumerating PCI bus instead:\n");
    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    for (int i = 0; i < n; i++) {
        kprintf("  %x:%x.%x  vendor %x  device %x  ",
                pds[i].bus, pds[i].slot, pds[i].func, pds[i].vendor, pds[i].device);
        sh_pad(pci_class_name(pds[i].class), 14);
        kprintf("\n");
    }
    kprintf("%d device(s)\n", n);
    return 0;
}

/* firewall: an in-RAM ruleset. It is real state you can edit/list, but with no
 * network stack it filters nothing - kept for when a NIC driver lands. */
#define FW_MAX 16
#define FW_LEN 48
static char fw_rules[FW_MAX][FW_LEN];
static int  fw_count;
static int  fw_enabled;

int cmd_firewall(int argc, char **argv) {
    const char *act = (argc > 1) ? argv[1] : "status";
    if (strcmp(act, "on") == 0)        { fw_enabled = 1; kprintf("firewall enabled\n"); }
    else if (strcmp(act, "off") == 0)  { fw_enabled = 0; kprintf("firewall disabled\n"); }
    else if (strcmp(act, "clear") == 0){ fw_count = 0;   kprintf("firewall rules cleared\n"); }
    else if (strcmp(act, "add") == 0) {
        if (argc < 3) { kprintf("usage: firewall add RULE\n"); return 1; }
        if (fw_count >= FW_MAX) { kprintf("firewall: rule table full\n"); return 1; }
        char *r = fw_rules[fw_count];
        uint32_t L = 0;
        for (int i = 2; i < argc && L < FW_LEN - 1; i++) {
            if (i > 2 && L < FW_LEN - 1) r[L++] = ' ';
            for (const char *s = argv[i]; *s && L < FW_LEN - 1; ) r[L++] = *s++;
        }
        r[L] = 0;
        fw_count++;
        kprintf("rule added (#%d)\n", fw_count);
    } else if (strcmp(act, "list") == 0 || strcmp(act, "status") == 0) {
        kprintf("firewall: %s, %d rule(s)\n", fw_enabled ? "ENABLED" : "disabled", fw_count);
        for (int i = 0; i < fw_count; i++) kprintf("  %d) %s\n", i + 1, fw_rules[i]);
        kprintf("(advisory: no NIC bound, so nothing is actually filtered)\n");
    } else {
        kprintf("usage: firewall [on|off|add RULE|list|clear]\n");
    }
    return 0;
}
