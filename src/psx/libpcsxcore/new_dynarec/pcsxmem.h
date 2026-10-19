
extern u32 zero_mem[0x1000/4];

void new_dyna_pcsx_mem_init(void);
void new_dyna_pcsx_mem_reset(void);
void new_dyna_pcsx_mem_load_state(void);
void new_dyna_pcsx_mem_isolate(int enable);
void new_dyna_pcsx_mem_shutdown(void);
// ReSys12: map (mem != NULL) or route to the System 12 board handlers (mem == NULL) a read window at physical phys
void new_dyna_pcsx_mem_map_read(u32 phys, u32 size, void *mem);

int pcsxmem_is_handler_dynamic(unsigned int addr);
