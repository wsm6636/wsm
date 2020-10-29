#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <litmus.h>

#define PERIOD ms2ns(100)
#define DEADLINE ms2ns(100)
#define EXEC_COST ms2ns(20)
#define MEM_BUDGET 100
#define CALL(exp) do{\
	int ret;\
	ret=exp;\
	if(ret!=0)\
		fprintf(stderr,"%s failed: %m\n", #exp);\
	else\
		fprintf(stderr,"%s ok.\n", #exp);\
}while(0)

int i=0;
int job (void){
	i++;
	if(i>=10)
		return 1;
	return 0;
}

int main(){
	int do_exit;
	struct rt_task param;
	printf("%d",PERIOD);
	init_rt_task_param(&param);
	param.period=PERIOD;
	param.exec_cost=EXEC_COST;
	param.relative_deadline=DEADLINE;
	param.budget_policy=NO_ENFORCEMENT;
	param.mem_budget_task=MEM_BUDGET;
	CALL(init_litmus());
	CALL(set_rt_task_param(gettid(),&param));
	CALL(task_mode(LITMUS_RT_TASK));
	do{
		CALL(get_rt_task_param(gettid(),&param));
                printf("budget==%d\n",param.mem_budget_task);
		do_exit=job();
		sleep_next_period();
	}while(!do_exit);
	CALL(task_mode(BACKGROUND_TASK));
	return 0;
}
