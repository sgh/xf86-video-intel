#ifndef SNA_RENDER_H
#define SNA_RENDER_H

#define GRADIENT_CACHE_SIZE 16

#define fastcall __attribute__((regparm(3)))

struct sna;
struct sna_glyph;
struct sna_video;
struct sna_video_frame;

struct sna_composite_rectangles {
	struct sna_coordinate {
		int16_t x, y;
	} src, mask, dst;
	int16_t width, height;
};

struct sna_composite_op {
	fastcall void (*blt)(struct sna *sna, const struct sna_composite_op *op,
			     const struct sna_composite_rectangles *r);
	fastcall void (*box)(struct sna *sna,
			     const struct sna_composite_op *op,
			     const BoxRec *box);
	void (*boxes)(struct sna *sna, const struct sna_composite_op *op,
		      const BoxRec *box, int nbox);
	void (*done)(struct sna *sna, const struct sna_composite_op *op);

	struct sna_damage **damage;

	uint32_t op;

	struct {
		PixmapPtr pixmap;
		CARD32 format;
		struct kgem_bo *bo;
		int16_t x, y;
		uint16_t width, height;
	} dst;

	struct sna_composite_channel {
		struct kgem_bo *bo;
		PictTransform *transform;
		uint16_t width;
		uint16_t height;
		uint32_t pict_format;
		uint32_t card_format;
		uint32_t filter;
		uint32_t repeat;
		uint32_t is_affine : 1;
		uint32_t is_solid : 1;
		uint32_t is_opaque : 1;
		uint32_t alpha_fixup : 1;
		uint32_t rb_reversed : 1;
		int16_t offset[2];
		float scale[2];

		union {
			struct {
				uint32_t pixel;
			} gen2;
			struct gen3_shader_channel {
				int type;
				uint32_t mode;
				uint32_t constants;
			} gen3;
		} u;
	} src, mask;
	uint32_t is_affine : 1;
	uint32_t has_component_alpha : 1;
	uint32_t need_magic_ca_pass : 1;
	uint32_t rb_reversed : 1;

	int16_t floats_per_vertex;
	int16_t floats_per_rect;
	fastcall void (*prim_emit)(struct sna *sna,
				   const struct sna_composite_op *op,
				   const struct sna_composite_rectangles *r);

	struct sna_composite_redirect {
		struct kgem_bo *real_bo;
		BoxRec box;
	} redirect;

	union {
		struct sna_blt_state {
			PixmapPtr src_pixmap;
			int16_t sx, sy;

			uint32_t inplace :1;
			uint32_t overwrites:1;

			uint32_t cmd;
			uint32_t br13;
			uint32_t pitch[2];
			uint32_t pixel;
			struct kgem_bo *bo[2];
		} blt;

		struct {
			float constants[8];
			uint32_t num_constants;
		} gen3;

		struct {
			int wm_kernel;
			int ve_id;
		} gen4;

		struct {
			int wm_kernel;
			int ve_id;
		} gen5;

		struct {
			int wm_kernel;
			int nr_surfaces;
			int nr_inputs;
			int ve_id;
		} gen6;

		struct {
			int wm_kernel;
			int nr_surfaces;
			int nr_inputs;
			int ve_id;
		} gen7;

		void *priv;
	} u;
};

struct sna_composite_spans_op {
	struct sna_composite_op base;

	fastcall void (*box)(struct sna *sna,
			     const struct sna_composite_spans_op *op,
			     const BoxRec *box,
			     float opacity);
	void (*boxes)(struct sna *sna,
		      const struct sna_composite_spans_op *op,
		      const BoxRec *box, int nbox,
		      float opacity);
	fastcall void (*done)(struct sna *sna,
			      const struct sna_composite_spans_op *op);

	fastcall void (*prim_emit)(struct sna *sna,
				   const struct sna_composite_spans_op *op,
				   const BoxRec *box,
				   float opacity);
};

struct sna_fill_op {
	struct sna_composite_op base;

	void (*blt)(struct sna *sna, const struct sna_fill_op *op,
		    int16_t x, int16_t y, int16_t w, int16_t h);
	void (*done)(struct sna *sna, const struct sna_fill_op *op);
};

struct sna_copy_op {
	struct sna_composite_op base;

	void (*blt)(struct sna *sna, const struct sna_copy_op *op,
		    int16_t sx, int16_t sy,
		    int16_t w, int16_t h,
		    int16_t dx, int16_t dy);
	void (*done)(struct sna *sna, const struct sna_copy_op *op);
};

struct sna_render {
	int max_3d_size;

	Bool (*composite)(struct sna *sna, uint8_t op,
			  PicturePtr dst, PicturePtr src, PicturePtr mask,
			  int16_t src_x, int16_t src_y,
			  int16_t msk_x, int16_t msk_y,
			  int16_t dst_x, int16_t dst_y,
			  int16_t w, int16_t h,
			  struct sna_composite_op *tmp);

	Bool (*composite_spans)(struct sna *sna, uint8_t op,
				PicturePtr dst, PicturePtr src,
				int16_t src_x, int16_t src_y,
				int16_t dst_x, int16_t dst_y,
				int16_t w, int16_t h,
				struct sna_composite_spans_op *tmp);

	Bool (*video)(struct sna *sna,
		      struct sna_video *video,
		      struct sna_video_frame *frame,
		      RegionPtr dstRegion,
		      short src_w, short src_h,
		      short drw_w, short drw_h,
		      PixmapPtr pixmap);

	Bool (*fill_boxes)(struct sna *sna,
			   CARD8 op,
			   PictFormat format,
			   const xRenderColor *color,
			   PixmapPtr dst, struct kgem_bo *dst_bo,
			   const BoxRec *box, int n);
	Bool (*fill)(struct sna *sna, uint8_t alu,
		     PixmapPtr dst, struct kgem_bo *dst_bo,
		     uint32_t color,
		     struct sna_fill_op *tmp);
	Bool (*fill_one)(struct sna *sna, PixmapPtr dst, struct kgem_bo *dst_bo,
			 uint32_t color,
			 int16_t x1, int16_t y1, int16_t x2, int16_t y2,
			 uint8_t alu);

	Bool (*copy_boxes)(struct sna *sna, uint8_t alu,
			   PixmapPtr src, struct kgem_bo *src_bo, int16_t src_dx, int16_t src_dy,
			   PixmapPtr dst, struct kgem_bo *dst_bo, int16_t dst_dx, int16_t dst_dy,
			   const BoxRec *box, int n);
	Bool (*copy)(struct sna *sna, uint8_t alu,
		     PixmapPtr src, struct kgem_bo *src_bo,
		     PixmapPtr dst, struct kgem_bo *dst_bo,
		     struct sna_copy_op *op);

	void (*flush)(struct sna *sna);
	void (*reset)(struct sna *sna);
	void (*fini)(struct sna *sna);

	struct sna_alpha_cache {
		struct kgem_bo *cache_bo;
		struct kgem_bo *bo[256];
	} alpha_cache;

	struct sna_solid_cache {
		struct kgem_bo *cache_bo;
		uint32_t color[1024];
		struct kgem_bo *bo[1024];
		int last;
		int size;
		int dirty;
	} solid_cache;

	struct {
		struct sna_gradient_cache {
			struct kgem_bo *bo;
			int nstops;
			PictGradientStop *stops;
		} cache[GRADIENT_CACHE_SIZE];
		int size;
	} gradient_cache;

	struct sna_glyph_cache{
		PicturePtr picture;
		struct sna_glyph **glyphs;
		uint16_t count;
		uint16_t evict;
	} glyph[2];

	uint16_t vertex_start;
	uint16_t vertex_index;
	uint16_t vertex_used;
	uint16_t vertex_reloc[8];

	float vertex_data[16*1024];
	const struct sna_composite_op *op;
};

struct gen2_render_state {
	uint32_t target;
	Bool need_invariant;
	Bool logic_op_enabled;
	uint32_t ls1, ls2, vft;
	uint16_t vertex_offset;
	uint32_t diffuse;
};

struct gen3_render_state {
	uint32_t current_dst;
	Bool need_invariant;
	uint32_t tex_count;
	uint32_t last_drawrect_limit;
	uint32_t last_target;
	uint32_t last_blend;
	uint32_t last_constants;
	uint32_t last_sampler;
	uint32_t last_shader;
	uint32_t last_diffuse;
	uint32_t last_specular;

	uint16_t vertex_offset;
	uint16_t last_vertex_offset;
	uint16_t floats_per_vertex;
	uint16_t last_floats_per_vertex;

	uint32_t tex_map[4];
	uint32_t tex_handle[2];
	uint32_t tex_delta[2];
};

struct gen4_render_state {
	struct kgem_bo *general_bo;

	uint32_t vs;
	uint32_t sf[2];
	uint32_t wm;
	uint32_t cc;

	int ve_id;
	uint32_t drawrect_offset;
	uint32_t drawrect_limit;
	uint32_t vb_id;
	uint16_t vertex_offset;
	uint16_t last_primitive;
	int16_t floats_per_vertex;
	uint16_t surface_table;
	uint16_t last_pipelined_pointers;

	Bool needs_invariant;
	Bool needs_urb;
};

struct gen5_render_state {
	struct kgem_bo *general_bo;

	uint32_t vs;
	uint32_t sf[2];
	uint32_t wm;
	uint32_t cc;

	int ve_id;
	uint32_t drawrect_offset;
	uint32_t drawrect_limit;
	uint32_t vb_id;
	uint16_t vertex_offset;
	uint16_t last_primitive;
	int16_t floats_per_vertex;
	uint16_t surface_table;
	uint16_t last_pipelined_pointers;

	Bool needs_invariant;
};

enum {
	GEN6_WM_KERNEL_NOMASK = 0,
	GEN6_WM_KERNEL_NOMASK_PROJECTIVE,

	GEN6_WM_KERNEL_MASK,
	GEN6_WM_KERNEL_MASK_PROJECTIVE,

	GEN6_WM_KERNEL_MASKCA,
	GEN6_WM_KERNEL_MASKCA_PROJECTIVE,

	GEN6_WM_KERNEL_MASKCA_SRCALPHA,
	GEN6_WM_KERNEL_MASKCA_SRCALPHA_PROJECTIVE,

	GEN6_WM_KERNEL_VIDEO_PLANAR,
	GEN6_WM_KERNEL_VIDEO_PACKED,
	GEN6_KERNEL_COUNT
};

struct gen6_render_state {
	struct kgem_bo *general_bo;

	uint32_t vs_state;
	uint32_t sf_state;
	uint32_t sf_mask_state;
	uint32_t wm_state;
	uint32_t wm_kernel[GEN6_KERNEL_COUNT];

	uint32_t cc_vp;
	uint32_t cc_blend;

	uint32_t drawrect_offset;
	uint32_t drawrect_limit;
	uint32_t blend;
	uint32_t samplers;
	uint32_t kernel;

	uint16_t num_sf_outputs;
	uint16_t vb_id;
	uint16_t ve_id;
	uint16_t vertex_offset;
	uint16_t last_primitive;
	int16_t floats_per_vertex;
	uint16_t surface_table;

	Bool needs_invariant;
};

enum {
	GEN7_WM_KERNEL_NOMASK = 0,
	GEN7_WM_KERNEL_NOMASK_PROJECTIVE,

	GEN7_WM_KERNEL_MASK,
	GEN7_WM_KERNEL_MASK_PROJECTIVE,

	GEN7_WM_KERNEL_MASKCA,
	GEN7_WM_KERNEL_MASKCA_PROJECTIVE,

	GEN7_WM_KERNEL_MASKCA_SRCALPHA,
	GEN7_WM_KERNEL_MASKCA_SRCALPHA_PROJECTIVE,

	GEN7_WM_KERNEL_VIDEO_PLANAR,
	GEN7_WM_KERNEL_VIDEO_PACKED,
	GEN7_KERNEL_COUNT
};

struct gen7_render_state {
	struct kgem_bo *general_bo;

	uint32_t vs_state;
	uint32_t sf_state;
	uint32_t sf_mask_state;
	uint32_t wm_state;
	uint32_t wm_kernel[GEN6_KERNEL_COUNT];

	uint32_t cc_vp;
	uint32_t cc_blend;

	uint32_t drawrect_offset;
	uint32_t drawrect_limit;
	uint32_t blend;
	uint32_t samplers;
	uint32_t kernel;

	uint16_t num_sf_outputs;
	uint16_t vb_id;
	uint16_t ve_id;
	uint16_t vertex_offset;
	uint16_t last_primitive;
	int16_t floats_per_vertex;
	uint16_t surface_table;

	Bool needs_invariant;
};

struct sna_static_stream {
	uint32_t size, used;
	uint8_t *data;
};

int sna_static_stream_init(struct sna_static_stream *stream);
uint32_t sna_static_stream_add(struct sna_static_stream *stream,
			       const void *data, uint32_t len, uint32_t align);
void *sna_static_stream_map(struct sna_static_stream *stream,
			    uint32_t len, uint32_t align);
uint32_t sna_static_stream_offsetof(struct sna_static_stream *stream,
				    void *ptr);
struct kgem_bo *sna_static_stream_fini(struct sna *sna,
				       struct sna_static_stream *stream);

struct kgem_bo *
sna_render_get_solid(struct sna *sna,
		     uint32_t color);

void
sna_render_flush_solid(struct sna *sna);

struct kgem_bo *
sna_render_get_gradient(struct sna *sna,
			PictGradient *pattern);

uint32_t sna_rgba_for_color(uint32_t color, int depth);
Bool sna_picture_is_solid(PicturePtr picture, uint32_t *color);

void no_render_init(struct sna *sna);

Bool gen2_render_init(struct sna *sna);
Bool gen3_render_init(struct sna *sna);
Bool gen4_render_init(struct sna *sna);
Bool gen5_render_init(struct sna *sna);
Bool gen6_render_init(struct sna *sna);
Bool gen7_render_init(struct sna *sna);

Bool sna_tiling_composite(uint32_t op,
			  PicturePtr src,
			  PicturePtr mask,
			  PicturePtr dst,
			  int16_t src_x, int16_t src_y,
			  int16_t mask_x, int16_t mask_y,
			  int16_t dst_x, int16_t dst_y,
			  int16_t width, int16_t height,
			  struct sna_composite_op *tmp);

Bool sna_blt_composite(struct sna *sna,
		       uint32_t op,
		       PicturePtr src,
		       PicturePtr dst,
		       int16_t src_x, int16_t src_y,
		       int16_t dst_x, int16_t dst_y,
		       int16_t width, int16_t height,
		       struct sna_composite_op *tmp);

bool sna_blt_fill(struct sna *sna, uint8_t alu,
		  struct kgem_bo *bo,
		  int bpp,
		  uint32_t pixel,
		  struct sna_fill_op *fill);

bool sna_blt_copy(struct sna *sna, uint8_t alu,
		  struct kgem_bo *src,
		  struct kgem_bo *dst,
		  int bpp,
		  struct sna_copy_op *copy);

Bool sna_blt_fill_boxes(struct sna *sna, uint8_t alu,
			struct kgem_bo *bo,
			int bpp,
			uint32_t pixel,
			const BoxRec *box, int n);

Bool sna_blt_copy_boxes(struct sna *sna, uint8_t alu,
			struct kgem_bo *src_bo, int16_t src_dx, int16_t src_dy,
			struct kgem_bo *dst_bo, int16_t dst_dx, int16_t dst_dy,
			int bpp,
			const BoxRec *box, int n);

Bool sna_get_pixel_from_rgba(uint32_t *pixel,
			     uint16_t red,
			     uint16_t green,
			     uint16_t blue,
			     uint16_t alpha,
			     uint32_t format);

int
sna_render_pixmap_bo(struct sna *sna,
		     struct sna_composite_channel *channel,
		     PixmapPtr pixmap,
		     int16_t x, int16_t y,
		     int16_t w, int16_t h,
		     int16_t dst_x, int16_t dst_y);

int
sna_render_picture_extract(struct sna *sna,
			   PicturePtr picture,
			   struct sna_composite_channel *channel,
			   int16_t x, int16_t y,
			   int16_t w, int16_t h,
			   int16_t dst_x, int16_t dst_y);

int
sna_render_picture_fixup(struct sna *sna,
			 PicturePtr picture,
			 struct sna_composite_channel *channel,
			 int16_t x, int16_t y,
			 int16_t w, int16_t h,
			 int16_t dst_x, int16_t dst_y);

int
sna_render_picture_convert(struct sna *sna,
			   PicturePtr picture,
			   struct sna_composite_channel *channel,
			   PixmapPtr pixmap,
			   int16_t x, int16_t y,
			   int16_t w, int16_t h,
			   int16_t dst_x, int16_t dst_y);

Bool
sna_render_composite_redirect(struct sna *sna,
			      struct sna_composite_op *op,
			      int x, int y, int width, int height);

void
sna_render_composite_redirect_done(struct sna *sna,
				   const struct sna_composite_op *op);

#endif /* SNA_RENDER_H */
