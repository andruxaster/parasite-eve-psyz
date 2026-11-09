/* Sampling profiler for the Dreamcast backend (dc_prof.c). */
#ifndef PSYZ_DC_PROF_H
#define PSYZ_DC_PROF_H

void DcProf_Start(void);
/* logs the busiest code since the last report, then starts over */
void DcProf_Report(void);

#endif
