/*
 * Collection and parsing of the MPEG/DVB service information of a TS
 * (PAT, PMT, SDT actual, NIT actual) for dvbrx's channel probes: services
 * with their names and whether they are scrambled, multiplexes announced
 * by the NIT.
 */
#ifndef DVBRX_PSI_H
#define DVBRX_PSI_H

#include <stdint.h>

#define PSI_MAX_SERVICES 64
#define PSI_MAX_MUXES 128

struct psi_service {
    int sid;
    int type;          /* SDT service_type, -1 if unknown */
    int scrambled;     /* its audio/video packets mostly scrambled */
    char name[64];     /* UTF-8 */
    char provider[64]; /* UTF-8 */
};

struct psi_mux {
    char delsys[8]; /* "DVB-C", "DVB-T", "DVB-T2", "DVB-S", "DVB-S2" */
    double freq;    /* Hz (satellite: RF frequency) */
    double symbol_rate, bandwidth;
    int modulation; /* QAM order (DVB-C), 0 unknown */
    int tsid, polarization; /* 'H', 'V', 'L', 'R' or 0 */
    int plp;
};

struct psi_result {
    int tsid, onid; /* -1 if unknown */
    char network[64];
    int nservices, nmuxes;
    struct psi_service services[PSI_MAX_SERVICES];
    struct psi_mux muxes[PSI_MAX_MUXES];
};

typedef struct psi psi;

psi* psi_new(void);
void psi_free(psi* p);
/* Feed TS packets (len a multiple of 188). */
void psi_feed(psi* p, const uint8_t* ts, int len);
void psi_result_get(const psi* p, struct psi_result* r);

#endif
