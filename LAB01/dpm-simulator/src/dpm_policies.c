#include "inc/dpm_policies.h"
#include <math.h>

//#define T_Pre_N_Reg
#define T_Pre_Poly
#define T_B_E_I  T_on_off_I * T_off_on_I
#define T_B_E_S   T_on_off_S * T_off_on_S
//#define T_Pre_N_Reg
//#define one_state_only
//#define three_state
#define History

int dpm_simulate(psm_t psm, dpm_policy_t sel_policy, dpm_timeout_params
		tparams, dpm_history_params hparams, char* fwl)
{
    dpm_work_item *work_queue;
    psm_state_t curr_state = PSM_STATE_RUN;
    psm_state_t prev_state = PSM_STATE_RUN;
    psm_energy_t e_total = 0;
    psm_energy_t e_tran_total = 0;
    psm_energy_t e_total_no_dpm = 0;
    psm_time_t t_curr = 0;
    psm_time_t t_inactive_start = 0;
    psm_time_t t_tran_total = 0;
    psm_time_t t_waiting = 0;
	psm_time_t t_inactive_ideal = 0;
    psm_time_t t_active_ideal = 0;
    psm_time_t t_total_no_dpm = 0;
    psm_time_t t_state[PSM_N_STATES] = {0};
    psm_time_t history[DPM_HIST_WIND_SIZE];
    int n_tran_total;
    int next_work_item;
    int num_work_items;

    // creates the queue of work items to be executed by the simulated system
    work_queue = dpm_init_work_queue(&num_work_items, fwl);
    if (work_queue == NULL){
        return 0;
    }

    // compute baseline results (energy without DPM and ideal active/inactive times)
    t_curr = 0;
    for (int i = 0; i < num_work_items; i++) {
        // 1. Inactive phase
        psm_time_t t_inactive = work_queue[i].arrival - t_curr;
        t_inactive_ideal += t_inactive;
        t_total_no_dpm += t_inactive;
        // no DPM --> we stay in RUN state even when inactive
        e_total_no_dpm += psm_state_energy(psm, PSM_STATE_RUN, t_inactive);
        t_curr += t_inactive;
        // 2. Active phase
        psm_time_t t_active = work_queue[i].duration;
        t_active_ideal += t_active;
        t_total_no_dpm += t_active;
        e_total_no_dpm += psm_state_energy(psm, PSM_STATE_RUN, t_active);
        t_curr += t_active;
    }

    // DPM Simulation loop
    t_curr = 0;
    n_tran_total = 0;
    next_work_item = 0;
    dpm_init_history(history);

    while (next_work_item < num_work_items) {

        // 1. Inactive phase
        t_inactive_start = t_curr;
        while(t_curr < work_queue[next_work_item].arrival) {
            if (!dpm_decide_state(&curr_state, prev_state, t_curr, t_inactive_start, history, sel_policy, tparams, hparams)) {
                printf("[error] cannot decide next state!\n");
                return 0;
            }
            if (curr_state != prev_state) {
                if(!psm_tran_allowed(psm, prev_state, curr_state)) {
                    printf("[error] prohibited transition!\n");
                    return 0;
                }
                psm_energy_t e_tran = psm_tran_energy(psm, prev_state, curr_state);
                psm_time_t t_tran = psm_tran_time(psm, prev_state, curr_state);
                n_tran_total++;
                e_tran_total += e_tran;
                e_total += e_tran;
                t_tran_total += t_tran;
                t_curr += t_tran;
            } else {
                // spend one simulation time-step in the current state, then re-evaluate
                psm_time_t time_unit = SIMULATION_TIME_STEP / PSM_TIME_UNIT;
                e_total += psm_state_energy(psm, curr_state, time_unit);
                t_curr += time_unit;
                t_state[curr_state] += time_unit;
                // time spent in RUN while there was no work to be done
                if(curr_state == PSM_STATE_RUN)
                    t_waiting += time_unit;
            }
            prev_state = curr_state;
        }
        // update history based on last inactive time (this can be placed elsewhere depending on your policy)
        dpm_update_history(history, t_curr - t_inactive_start); 

        // 2. Active phase
        curr_state = PSM_STATE_RUN;
        if (curr_state != prev_state) {
            if(!psm_tran_allowed(psm, prev_state, curr_state)) {
                printf("[error] prohibited transition!\n");
                return 0;
            }
            n_tran_total++;
            psm_energy_t e_tran = psm_tran_energy(psm, prev_state, curr_state);
            psm_time_t t_tran = psm_tran_time(psm, prev_state, curr_state);
            e_tran_total += e_tran;
            e_total += e_tran;
            t_tran_total += t_tran;
            t_curr += t_tran;
            prev_state = PSM_STATE_RUN;
        }
        // do the queued work (there could be more than one item queued due to accumulated delays)
        while(next_work_item < num_work_items && t_curr >= work_queue[next_work_item].arrival) {
            t_curr += work_queue[next_work_item].duration;
            t_state[curr_state] += work_queue[next_work_item].duration;
            e_total += psm_state_energy(psm, curr_state, work_queue[next_work_item].duration);
            next_work_item++;
        }
    }
    free(work_queue);

    printf("[sim] Active time in profile = %.6lfs \n", t_active_ideal * PSM_TIME_UNIT);
    printf("[sim] Inactive time in profile = %.6lfs\n", t_inactive_ideal * PSM_TIME_UNIT);
    printf("[sim] Tot. Time w/o DPM = %.6lfs, Tot. Time w DPM = %.6lfs\n",
           t_total_no_dpm * PSM_TIME_UNIT, t_curr * PSM_TIME_UNIT);
    for(int i = 0; i < PSM_N_STATES; i++) {
        printf("[sim] Total time in state %s = %.6lfs\n", PSM_STATE_NAME(i),
                t_state[i] * PSM_TIME_UNIT);
    }
    printf("[sim] Timeout waiting time = %.6lfs\n", t_waiting * PSM_TIME_UNIT);
    printf("[sim] Transitions time = %.6lfs\n", t_tran_total * PSM_TIME_UNIT);
    printf("[sim] N. of transitions = %d\n", n_tran_total);
    printf("[sim] Energy for transitions = %.10fJ\n", e_tran_total * PSM_ENERGY_UNIT);
    printf("[sim] Tot. Energy w/o DPM = %.10fJ, Tot. Energy w DPM = %.10fJ\n",
            e_total_no_dpm * PSM_ENERGY_UNIT, e_total * PSM_ENERGY_UNIT);
	return 1;
}

/* decide next power state */
int dpm_decide_state(psm_state_t *next_state, psm_state_t prev_state, psm_time_t t_curr,
        psm_time_t t_inactive_start, psm_time_t *history, dpm_policy_t policy,
        dpm_timeout_params tparams, dpm_history_params hparams)
{
    psm_time_t  T_Pre = 0;
    psm_time_t  Thr_Sleep = hparams.threshold[0];
    psm_time_t  Thr_Idle  = hparams.threshold[1];


    switch (policy) {

        case DPM_TIMEOUT:
    
                #ifdef one_state_only 
               //one transition only (RUN>>SLEEP or SLEEP>>RUN) or (RUN>>IDLE or IDLE>>RUN)
               if((t_curr > t_inactive_start + tparams.timeout[1]) &&  (tparams.timeout[1] >  tparams.timeout[0]))
                 *next_state = PSM_STATE_SLEEP;

               else if((t_curr > t_inactive_start + tparams.timeout[0]) &&  (tparams.timeout[1] <  tparams.timeout[0]))
                *next_state = PSM_STATE_IDLE;

               else {
                *next_state = PSM_STATE_RUN;}
               break;
               #endif

               
               #ifdef three_state
             // three state transition between idle>>run>>sleep and sleep>>run>>idle  without any violation between sleep and idle   
               if(tparams.timeout[1] >= tparams.timeout[0] && (t_curr > t_inactive_start + tparams.timeout[1])) 
                 {
                        if((prev_state == PSM_STATE_SLEEP) || (prev_state == PSM_STATE_RUN) )
                     {

                        *next_state = PSM_STATE_SLEEP;
                     }
                        else {
                        *next_state = PSM_STATE_RUN;}
                 }

               else if((t_curr > t_inactive_start + tparams.timeout[0]))
                 {
                 
                   if((prev_state == PSM_STATE_IDLE) || (prev_state == PSM_STATE_RUN) )
                     {

                         *next_state = PSM_STATE_IDLE;
                     }
                   else {
                         *next_state = PSM_STATE_RUN;}
                  }
                 
            else {
                *next_state = PSM_STATE_RUN;
            }
            break;
            #endif 


        //Day 3: EDIT
        #ifdef History 
        case DPM_HISTORY:
             if(t_curr > t_inactive_start)
                {
		#ifdef T_Pre_Poly
		    for( int i =0; i < DPM_HIST_WIND_SIZE; i++)
		    {
		     T_Pre += hparams.alpha[i] * pow(history[i],i);
		    }
		    #endif

		#ifdef T_Pre_N_Reg 
		    for( int i =1; i < DPM_HIST_WIND_SIZE; i++)
		    {
		     T_Pre += hparams.alpha[i]*(history[DPM_HIST_WIND_SIZE-(i)]);
		    }
		     T_Pre += hparams.alpha[0];
		   #endif 
             if((T_Pre >= T_B_E_S) && (T_Pre >= Thr_Sleep) && (Thr_Sleep > Thr_Idle )) 
                 {
                        if((prev_state == PSM_STATE_SLEEP) || (prev_state == PSM_STATE_RUN) )
                     {

                        *next_state = PSM_STATE_SLEEP;
                     }
                        else {
                        *next_state = PSM_STATE_RUN;}
                 }

               else if((T_Pre >= T_B_E_I) && (T_Pre >= Thr_Idle))
                 {
                 
                   if((prev_state == PSM_STATE_IDLE) || (prev_state == PSM_STATE_RUN) )
                     {

                         *next_state = PSM_STATE_IDLE;
                     }
                   else {
                         *next_state = PSM_STATE_RUN;}
                  }
                 }

            else {
                *next_state = PSM_STATE_RUN;
            }          
            break;
            #endif

        default:
            printf("[error] unsupported policy\n");
            return 0;
    }
	return 1;
}

/* initialize inactive time history */
void dpm_init_history(psm_time_t *h)
{
	for (int i=0; i<DPM_HIST_WIND_SIZE; i++) {
		h[i] = 0;
	}
}

/* update inactive time history */
void dpm_update_history(psm_time_t *h, psm_time_t new_inactive)
{
	for (int i=0; i<DPM_HIST_WIND_SIZE-1; i++){
		h[i] = h[i+1];
	}
	h[DPM_HIST_WIND_SIZE-1] = new_inactive;
}

/* initialize work queue */
dpm_work_item *dpm_init_work_queue(int *num_items, char *fwl) {
    FILE *fp;
    int n_lines;

    fp = fopen(fwl, "r");
    if (!fp) {
        printf("[error] Can't open workload file %s!\n", fwl);
		return NULL;
    }

    // compute number of work items to allocate correctly sized array
    n_lines = 0;
    while (fscanf(fp, "%*lf%*lf") != EOF)
        n_lines++;
    rewind(fp);

    dpm_work_item *work_queue = (dpm_work_item*) malloc(sizeof(dpm_work_item)*n_lines);
    work_queue[0].arrival = 0;
    int i = 0;
    while (fscanf(fp, "%lf%lf", &work_queue[i].arrival, &work_queue[i].duration) == 2)
        i++;
    fclose(fp);
    *num_items = i;
    return work_queue;
}
