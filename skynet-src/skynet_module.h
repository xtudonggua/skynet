#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);
typedef void (*skynet_dl_signal)(void * inst, int signal);

struct skynet_module { //单个模块结构
	const char * name; //c服务名称，一般是指c服务的文件名
	void * module; //访问so库的dl句柄，通过dlopen获得该句柄
	skynet_dl_create create; //通过dlsym绑定so库中的xxx_create函数，调用create即调用xxx_create接口
	skynet_dl_init init; //绑定xxx_init接口
	skynet_dl_release release; //绑定xxx_release接口
	skynet_dl_signal signal; //绑定xxx_signal接口
};

void skynet_module_insert(struct skynet_module *mod);
struct skynet_module * skynet_module_query(const char * name);
void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

void skynet_module_init(const char *path);

#endif
