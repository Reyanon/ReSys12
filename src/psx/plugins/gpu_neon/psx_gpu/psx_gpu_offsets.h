#ifndef __P_PSX_GPU_OFFSETS_H__
#define __P_PSX_GPU_OFFSETS_H__

/* ReSys12: regenerated (psx_gpu_offsets_update.c, arm-linux-gnueabihf) after
   the texture page masks became u64 and moved behind the enhancement fields */

#define psx_gpu_uvrg_offset                               0x10
#define psx_gpu_uvrg_dx_offset                            0x20
#define psx_gpu_uvrg_dy_offset                            0x30
#define psx_gpu_u_block_span_offset                       0x40
#define psx_gpu_v_block_span_offset                       0x50
#define psx_gpu_r_block_span_offset                       0x60
#define psx_gpu_g_block_span_offset                       0x70
#define psx_gpu_b_block_span_offset                       0x80
#define psx_gpu_b_offset                                  0x90
#define psx_gpu_b_dy_offset                               0x94
#define psx_gpu_triangle_area_offset                      0x98
#define psx_gpu_current_texture_mask_offset               0x138
#define psx_gpu_dirty_textures_4bpp_mask_offset           0x148
#define psx_gpu_dirty_textures_8bpp_mask_offset           0x150
#define psx_gpu_dirty_textures_8bpp_alternate_mask_offset 0x158
#define psx_gpu_triangle_color_offset                     0xa0
#define psx_gpu_dither_table_offset                       0xa4
#define psx_gpu_texture_page_ptr_offset                   0xb8
#define psx_gpu_texture_page_base_offset                  0xbc
#define psx_gpu_clut_ptr_offset                           0xc0
#define psx_gpu_vram_ptr_offset                           0xc4
#define psx_gpu_vram_out_ptr_offset                       0xc8
#define psx_gpu_uvrgb_phase_offset                        0xcc
#define psx_gpu_num_spans_offset                          0xd4
#define psx_gpu_num_blocks_offset                         0xd6
#define psx_gpu_viewport_start_x_offset                   0xd8
#define psx_gpu_viewport_start_y_offset                   0xda
#define psx_gpu_viewport_end_x_offset                     0xdc
#define psx_gpu_viewport_end_y_offset                     0xde
#define psx_gpu_mask_msb_offset                           0xe0
#define psx_gpu_triangle_winding_offset                   0xe2
#define psx_gpu_current_texture_page_offset               0xe4
#define psx_gpu_texture_mask_width_offset                 0xe6
#define psx_gpu_texture_mask_height_offset                0xe7
#define psx_gpu_reciprocal_table_ptr_offset               0xf4
#define psx_gpu_hacks_active_offset                       0x100
#define psx_gpu_saved_tmp_offset                          0x190
#define psx_gpu_blocks_offset                             0x200
#define psx_gpu_span_uvrg_offset_offset                   0x2200
#define psx_gpu_span_edge_data_offset                     0x4200
#define psx_gpu_span_b_offset_offset                      0x5200

#endif /* __P_PSX_GPU_OFFSETS_H__ */
