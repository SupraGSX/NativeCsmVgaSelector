#ifndef NCV_PCI_ID_LOOKUP_H
#define NCV_PCI_ID_LOOKUP_H
/* Bounded parser of pci.ids vendor/device records; subsystem/class records ignored. */
int PciIdLookup(const char *Data, unsigned long long Size, unsigned Vendor,
                unsigned Device, char *Name, unsigned Capacity);
#endif
