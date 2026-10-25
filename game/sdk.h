/* The part of the Katana SDK's headers that tools/ghidra/analyze.sh uses: the
 * prototypes of the SDK functions that game/symbols.tsv names, and the types that
 * they and the game's own types need. tools/ghidra/sdk_header.sh writes it from the
 * SDK's headers. Do not edit. */

typedef unsigned char   undefined;

typedef unsigned char    uchar;
typedef unsigned int    uint;
typedef unsigned long    ulong;
typedef unsigned short    ushort;
typedef long Sint32;

typedef struct BUS_BACKUPFILEHEADER BUS_BACKUPFILEHEADER, *PBUS_BACKUPFILEHEADER;

typedef uchar Uint8;

typedef ushort Uint16;

typedef ulong Uint32;

struct BUS_BACKUPFILEHEADER {
    char vms_comment[18];
    char btr_comment[34];
    Uint8 game_name[16];
    void *icon_palette;
    void *icon_data;
    Uint16 icon_num;
    Uint16 icon_speed;
    void *visual_data;
    Uint16 visual_type;
    Uint16 reserved;
    void *save_data;
    Uint32 save_size;
};

typedef struct BUS_TIME BUS_TIME, *PBUS_TIME;

struct BUS_TIME {
    Uint16 year;
    Uint8 month;
    Uint8 day;
    Uint8 hour;
    Uint8 minute;
    Uint8 second;
    Uint8 dayofweek;
};

typedef void (*BU_INITCALLBACK)(void);

typedef Sint32 (*BU_COMPLETECALLBACK)(Sint32, Sint32, Sint32, Uint32);

typedef ulong size_t;

typedef struct GDS_FS_HANDLE GDS_FS_HANDLE, *PGDS_FS_HANDLE;

typedef struct GDS_FS_HANDLE GDFS_HANDLE;

typedef GDFS_HANDLE *GDFS;

typedef struct GDS_FS_WORK GDS_FS_WORK, *PGDS_FS_WORK;

typedef struct GDS_FS_WORK GDFS_WORK;

typedef short Sint16;

typedef struct GDS_DCF GDS_DCF, *PGDS_DCF;

typedef struct GDS_DCF GD_DCF;

typedef struct GDS_FS_DIRREC_TBL GDS_FS_DIRREC_TBL, *PGDS_FS_DIRREC_TBL;

typedef struct GDS_FS_DIRREC_TBL GDFS_DIRREC_TBL;

typedef GDFS_DIRREC_TBL *GDFS_DIRREC;

typedef struct GDS_FS_LIF GDS_FS_LIF, *PGDS_FS_LIF;

typedef struct GDS_FS_LIF GDFS_LIF;

typedef struct GDS_FS_DIRREC_ENT GDS_FS_DIRREC_ENT, *PGDS_FS_DIRREC_ENT;

typedef struct GDS_FS_DIRREC_ENT GDFS_DIRREC_ENT;

typedef char Sint8;

struct GDS_FS_HANDLE {
    GDFS_WORK *wk;
    Sint32 fid;
    Sint32 fad;
    Sint32 fsize;
    Sint32 fsctsize;
    Sint32 ofs;
    Sint32 trnsed;
    Sint32 rsize;
    Sint32 trsize;
    void (*rdendcb)(void *);
    void *rdcb_1st;
    void (*trendcb)(void *);
    void *trcb_1st;
    void (*errcb)(void *, Sint32);
    void *errcb_1st;
    Sint32 gdchn;
    Sint32 gdchn_wait;
    Sint32 ex_errcode;
    Sint16 act;
    Sint16 trflag;
    Sint16 used;
    Sint16 tmode;
    Sint16 stat;
    Sint16 err;
};

struct GDS_FS_WORK {
    Sint32 max_open;
    Sint32 pathtbl_fad;
    Sint32 pathtbl_size;
    GD_DCF *dcf;
    GDFS_DIRREC curdir;
    GDFS syshdl;
    GDFS hndtbl;
    GDFS now_work;
    Sint32 f_svr;
    void (*g_errcb)(void *, Sint32);
    void *g_errcb_1st;
    Sint32 gdc_ver;
    Sint32 errstat;
    Sint32 istray;
    Sint16 f_init;
    Sint16 daplayed;
    GDFS_LIF *liftbl;
    Uint32 sctbuf[1040];
    GDFS_HANDLE hndlist[1];
};

struct GDS_DCF {
};

struct GDS_FS_DIRREC_TBL {
    Sint32 dir_num;
    Sint32 max_ent;
    Sint32 dir_fad;
    Sint32 pad;
    GDFS_DIRREC_ENT dirrec_tbl[1];
};

struct GDS_FS_LIF {
};

struct GDS_FS_DIRREC_ENT {
    Sint32 fad;
    Sint32 fsize;
    Uint8 flag;
    Uint8 sid;
    Sint8 fname[32];
    Sint8 pad[2];
};

typedef Sint32 Bool;

typedef struct GDS_FS_DAINFO GDS_FS_DAINFO, *PGDS_FS_DAINFO;

typedef struct GDS_FS_DAINFO GDFS_DAINFO;

struct GDS_FS_DAINFO {
    Sint32 track;
    Sint32 min;
    Sint32 sec;
    Sint32 frame;
    Sint32 fad;
};

typedef void (*GDFS_ERRFUNC)(void *, Sint32);

typedef void (*GDFS_FUNC)(void *);

typedef struct GDS_FS_DIRINFO GDS_FS_DIRINFO, *PGDS_FS_DIRINFO;

typedef struct GDS_FS_DIRINFO GDFS_DIRINFO;

struct GDS_FS_DIRINFO {
    Sint32 fad;
    Sint32 fsize;
    Uint8 flag;
    Uint8 pad[3];
};

typedef enum _tagKMSTATUS {
    KMSTATUS_SUCCESS=0,
    KMSTATUS_INVALID_DISPLAY_MODE=1,
    KMSTATUS_INVALID_VIDEO_MODE=2,
    KMSTATUS_NOT_ENOUGH_MEMORY=3,
    KMSTATUS_INVALID_TEXTURE_TYPE=4,
    KMSTATUS_INVALID_MIPMAPED_TEXTURE=5,
    KMSTATUS_CANT_FLIP_SURFACE=6,
    KMSTATUS_INVALID_ADDRESS=7,
    KMSTATUS_HARDWARE_NOT_PRESENTED=8,
    KMSTATUS_INVALID_SETTING=9,
    KMSTATUS_INVALID_VERTEX_TYPE=10,
    KMSTATUS_INVALID_LIST_TYPE=11,
    KMSTATUS_INVALID_BLOCKNUMBER=12,
    KMSTATUS_LAST_LIST_UNDER_DMA=13,
    KMSTATUS_INVALID_SURFACE=14,
    KMSTATUS_INVALID_SURFACE_TYPE=15,
    KMSTATUS_INVALID_SURFACE_SIZE=16,
    KMSTATUS_NOT_FINISH_DMA=17,
    KMSTATUS_OUT_OF_RANGE=18,
    KMSTATUS_UNDER_RENDER=102,
    KMSTATUS_FINISH_RENDER=103,
    KMSTATUS_UNDER_DISPLAY=104,
    KMSTATUS_FINISH_ALL_SEQUENCE=105,
    KMSTATUS_NOT_IMPLEMENTED=120,
    KMSTATUS_UNDER_DMA=256,
    KMSTATUS_FINISH_DMA=257,
    KMSTATUS_DMA_BUSY=258,
    KMSTATUS_QUEUE_EMPTY=259,
    KMSTATUS_INVALID_PARAMETER=56797,
    KMSTATUS_ERROR=61166
} _tagKMSTATUS;

typedef enum _tagKMSTATUS KMSTATUS;

typedef int KMINT32;

typedef struct _tagKMVERTEXBUFFDESC _tagKMVERTEXBUFFDESC, *P_tagKMVERTEXBUFFDESC;

typedef struct _tagKMVERTEXBUFFDESC *PKMVERTEXBUFFDESC;

typedef ulong DWORD;

typedef DWORD *PKMDWORD;

typedef struct _tagKMCURRENTLISTSTATE _tagKMCURRENTLISTSTATE, *P_tagKMCURRENTLISTSTATE;

typedef struct _tagKMCURRENTLISTSTATE *PKMCURRENTLISTSTATE;

typedef struct _tagKMVERTEXBUFFERPOINTER _tagKMVERTEXBUFFERPOINTER, *P_tagKMVERTEXBUFFERPOINTER;

typedef struct _tagKMVERTEXBUFFERPOINTER *PKMVERTEXBUFFERPOINTER;

typedef PKMVERTEXBUFFERPOINTER *PPKMVERTEXBUFFERPOINTER;

typedef ulong KMDWORD;

typedef struct _tagKMPASSINFO _tagKMPASSINFO, *P_tagKMPASSINFO;

typedef struct _tagKMPASSINFO *PKMPASSINFO;

typedef uint KMUINT32;

typedef float KMFLOAT;

struct _tagKMVERTEXBUFFDESC {
    PKMDWORD *pCurrentPtr;
    PKMDWORD pGlobalParam;
    PKMCURRENTLISTSTATE pCurrentListState;
    PPKMVERTEXBUFFERPOINTER ppBuffer;
    PKMDWORD *pFirstPassPtr;
    KMDWORD nMaxBank;
    PKMPASSINFO pPassInfo;
    KMDWORD Reserved3;
};

struct _tagKMCURRENTLISTSTATE {
    KMDWORD ListType;
    KMDWORD GlobalParamSize;
    KMDWORD VertexType;
    KMDWORD VertexBank;
    KMDWORD nCurrentPass;
    KMDWORD nMaxPass;
    KMDWORD flags;
};

struct _tagKMVERTEXBUFFERPOINTER {
    PKMDWORD pListBase[5];
};

struct _tagKMPASSINFO {
    KMDWORD dwRegionArrayFlag;
    PKMDWORD pVertexBuffer;
    KMUINT32 nVertexBufferSize;
    KMUINT32 nDirectTransferList;
    KMFLOAT fBufferSize[5];
    KMDWORD dwOPBSize[5];
};

typedef struct _tagKMSYSTEMCONFIGSTRUCT _tagKMSYSTEMCONFIGSTRUCT, *P_tagKMSYSTEMCONFIGSTRUCT;

typedef struct _tagKMSYSTEMCONFIGSTRUCT *PKMSYSTEMCONFIGSTRUCT;

typedef struct tagKMSURFACEDESC tagKMSURFACEDESC, *PtagKMSURFACEDESC;

typedef struct tagKMSURFACEDESC KMSURFACEDESC;

typedef KMSURFACEDESC *PKMSURFACEDESC;

typedef PKMSURFACEDESC *PPKMSURFACEDESC;

typedef struct _struct_105 _struct_105, *P_struct_105;

typedef struct _tagKMPASSINFO KMPASSINFO;

typedef union _union_96 _union_96, *P_union_96;

typedef union _union_97 _union_97, *P_union_97;

typedef union _union_98 _union_98, *P_union_98;

struct _struct_105 {
    KMUINT32 nNumOfFrameBuffer;
    KMUINT32 nStripBufferHeight;
};

union _union_96 {
    KMDWORD USize;
    KMDWORD Width;
};

union _union_97 {
    KMDWORD VSize;
    KMDWORD Height;
};

union _union_98 {
    KMDWORD dwTextureSize;
    KMDWORD dwFrameBufferSize;
};

struct _tagKMSYSTEMCONFIGSTRUCT {
    KMDWORD dwSize;
    KMDWORD flags;
    PPKMSURFACEDESC ppSurfaceDescArray;
    struct _struct_105 fb;
    KMUINT32 nTextureMemorySize;
    KMUINT32 nNumOfTextureStruct;
    KMUINT32 nNumOfSmallVQStruct;
    PKMDWORD pTextureWork;
    PKMVERTEXBUFFDESC pBufferDesc;
    KMUINT32 nNumOfVertexBank;
    PKMDWORD pVertexBuffer;
    KMUINT32 nVertexBufferSize;
    KMUINT32 nPassDepth;
    KMPASSINFO Pass[8];
};

struct tagKMSURFACEDESC {
    KMDWORD SurfaceType;
    KMDWORD BitDepth;
    KMDWORD PixelFormat;
    union _union_96 field3_0xc;
    union _union_97 field4_0x10;
    union _union_98 field5_0x14;
    KMDWORD fSurfaceFlags;
    PKMDWORD pSurface;
};

typedef int BOOL;

typedef BOOL KMBOOLEAN;

typedef int INT;

typedef INT *PKMINT32;

typedef uint UINT;

typedef UINT *PKMUINT32;

typedef struct _tagKMSYSTEMMETRICS _tagKMSYSTEMMETRICS, *P_tagKMSYSTEMMETRICS;

typedef struct _tagKMSYSTEMMETRICS *PKMSYSTEMMETRICS;

struct _tagKMSYSTEMMETRICS {
    KMDWORD flags;
    KMDWORD nLastRenderTime[8];
    KMDWORD nLastDMATime[8];
    KMDWORD nCurrentTimeOutCount;
    KMDWORD nDMATimeOutCount;
    KMDWORD nRenderTimeOutCount;
    KMDWORD nOBJOverflowCount;
    KMDWORD nParamOverflowCount;
    KMDWORD nParamCurrent;
    KMDWORD nOBJCurrent;
    KMDWORD nOBJLimit;
    KMDWORD nParamLimit;
    KMDWORD VertexBufferSize[8][5];
    KMDWORD MaxVertexSize[8][5];
    KMDWORD Reserved[16];
};

typedef enum enum_63 {
    KM_DREAMCAST=0,
    KM_NAOMI=65536
} enum_63;

typedef enum enum_63 KMDEVICE;

typedef struct _tagKMSTRIPHEAD _tagKMSTRIPHEAD, *P_tagKMSTRIPHEAD;

typedef struct _tagKMSTRIPHEAD *PKMSTRIPHEAD;

struct _tagKMSTRIPHEAD {
    KMDWORD dwStripHead[16];
};

typedef enum enum_89 {
    KM_VERTEXTYPE_00=0,
    KM_VERTEXTYPE_01=1,
    KM_VERTEXTYPE_02=2,
    KM_VERTEXTYPE_03=3,
    KM_VERTEXTYPE_04=4,
    KM_VERTEXTYPE_05=5,
    KM_VERTEXTYPE_06=6,
    KM_VERTEXTYPE_07=7,
    KM_VERTEXTYPE_08=8,
    KM_VERTEXTYPE_09=9,
    KM_VERTEXTYPE_10=10,
    KM_VERTEXTYPE_11=11,
    KM_VERTEXTYPE_12=12,
    KM_VERTEXTYPE_13=13,
    KM_VERTEXTYPE_14=14,
    KM_VERTEXTYPE_15=15,
    KM_VERTEXTYPE_16=16,
    KM_VERTEXTYPE_17=17
} enum_89;

typedef enum enum_89 KMVERTEXTYPE;

typedef void *PVOID;

typedef struct tagKMVERTEXCONTEXT tagKMVERTEXCONTEXT, *PtagKMVERTEXCONTEXT;

typedef struct tagKMVERTEXCONTEXT *PKMVERTEXCONTEXT;

typedef enum enum_73 {
    KM_POLYGON=0,
    KM_MODIFIERVOLUME=1,
    KM_SPRITE=2
} enum_73;

typedef enum enum_73 KMPARAMTYPE;

typedef enum enum_70 {
    KM_OPAQUE_POLYGON=0,
    KM_OPAQUE_MODIFIER=1,
    KM_TRANS_POLYGON=2,
    KM_TRANS_MODIFIER=3,
    KM_PUNCHTHROUGH_POLYGON=4
} enum_70;

typedef enum enum_70 KMLISTTYPE;

typedef enum enum_75 {
    KM_PACKEDCOLOR=0,
    KM_FLOATINGCOLOR=1,
    KM_INTENSITY=2,
    KM_INTENSITY_PREV_FACE_COL=3
} enum_75;

typedef enum enum_75 KMCOLORTYPE;

typedef enum enum_76 {
    KM_32BITUV=0,
    KM_16BITUV=1
} enum_76;

typedef enum enum_76 KMUVFORMAT;

typedef enum enum_78 {
    KM_IGNORE=0,
    KM_LESS=1,
    KM_EQUAL=2,
    KM_LESSEQUAL=3,
    KM_GREATER=4,
    KM_NOTEQUAL=5,
    KM_GREATEREQUAL=6,
    KM_ALWAYS=7
} enum_78;

typedef enum enum_78 KMDEPTHMODE;

typedef enum enum_79 {
    KM_NOCULLING=0,
    KM_CULLSMALL=1,
    KM_CULLCCW=2,
    KM_CULLCW=3
} enum_79;

typedef enum enum_79 KMCULLINGMODE;

typedef enum enum_80 {
    KM_NOTEXTUREFLAT=0,
    KM_NOTEXTUREGOURAUD=1,
    KM_TEXTUREFLAT=2,
    KM_TEXTUREGOURAUD=3
} enum_80;

typedef enum enum_80 KMSHADINGMODE;

typedef enum enum_77 {
    KM_NOMODIFIER=0,
    KM_MODIFIER_A=1
} enum_77;

typedef enum enum_77 KMMODIFIER;

typedef enum enum_82 {
    KM_BOTHINVSRCALPHA=0,
    KM_BOTHSRCALPHA=1,
    KM_DESTALPHA=2,
    KM_DESTCOLOR=3,
    KM_INVDESTALPHA=4,
    KM_INVDESTCOLOR=5,
    KM_INVSRCALPHA=6,
    KM_INVSRCCOLOR=7,
    KM_SRCALPHA=8,
    KM_SRCCOLOR=9,
    KM_ONE=10,
    KM_ZERO=11
} enum_82;

typedef enum enum_82 KMBLENDINGMODE;

typedef enum enum_81 {
    KM_FOGTABLE=0,
    KM_FOGVERTEX=1,
    KM_NOFOG=2,
    KM_FOGTABLE_2=3
} enum_81;

typedef enum enum_81 KMFOGMODE;

typedef enum enum_83 {
    KM_NOCLAMP=0,
    KM_CLAMP_V=1,
    KM_CLAMP_U=2,
    KM_CLAMP_UV=3
} enum_83;

typedef enum enum_83 KMCLAMPMODE;

typedef enum enum_84 {
    KM_NOFLIP=0,
    KM_FLIP_V=1,
    KM_FLIP_U=2,
    KM_FLIP_UV=3
} enum_84;

typedef enum enum_84 KMFLIPMODE;

typedef enum enum_85 {
    KM_POINT_SAMPLE=0,
    KM_BILINEAR=1,
    KM_TRILINEAR_A=2,
    KM_TRILINEAR_B=3
} enum_85;

typedef enum enum_85 KMFILTERMODE;

typedef enum enum_86 {
    KM_DECAL=0,
    KM_MODULATE=1,
    KM_DECAL_ALPHA=2,
    KM_MODULATE_ALPHA=3
} enum_86;

typedef enum enum_86 KMTEXTURESHADINGMODE;

typedef enum enum_88 {
    KM_USERCLIP_DISABLE=0,
    KM_USERCLIP_RESERVE=1,
    KM_USERCLIP_INSIDE=2,
    KM_USERCLIP_OUTSIDE=3
} enum_88;

typedef enum enum_88 KMUSERCLIPMODE;

struct tagKMVERTEXCONTEXT {
    KMDWORD RenderState;
    KMPARAMTYPE ParamType;
    KMLISTTYPE ListType;
    KMCOLORTYPE ColorType;
    KMUVFORMAT UVFormat;
    KMDEPTHMODE DepthMode;
    KMCULLINGMODE CullingMode;
    KMDWORD reserved00;
    KMSHADINGMODE ShadingMode;
    KMMODIFIER SelectModifier;
    KMBOOLEAN bZWriteDisable;
    KMBLENDINGMODE SRCBlendingMode;
    KMBLENDINGMODE DSTBlendingMode;
    KMBOOLEAN bSRCSel;
    KMBOOLEAN bDSTSel;
    KMFOGMODE FogMode;
    KMBOOLEAN bUseSpecular;
    KMBOOLEAN bUseAlpha;
    KMBOOLEAN bIgnoreTextureAlpha;
    KMCLAMPMODE ClampUV;
    KMFLIPMODE FlipUV;
    KMFILTERMODE FilterMode;
    KMBOOLEAN bSuperSample;
    KMDWORD MipMapAdjust;
    KMTEXTURESHADINGMODE TextureShadingMode;
    KMBOOLEAN bColorClamp;
    KMDWORD PaletteBank;
    PKMSURFACEDESC pTextureSurfaceDesc;
    KMFLOAT fFaceColorAlpha;
    KMFLOAT fFaceColorRed;
    KMFLOAT fFaceColorGreen;
    KMFLOAT fFaceColorBlue;
    KMFLOAT fOffsetColorAlpha;
    KMFLOAT fOffsetColorRed;
    KMFLOAT fOffsetColorGreen;
    KMFLOAT fOffsetColorBlue;
    KMDWORD GLOBALPARAMBUFFER;
    KMDWORD ISPPARAMBUFFER;
    KMDWORD TSPPARAMBUFFER;
    KMDWORD TexturePARAMBUFFER;
    KMDWORD ModifierInstruction;
    KMFLOAT reserved01;
    KMFLOAT reserved02;
    KMFLOAT reserved03;
    KMFLOAT reserved04;
    KMBOOLEAN bDCalcExact;
    KMDWORD reserved05;
    KMUSERCLIPMODE UserClipMode;
};

typedef union _tagKMPACKEDARGB _tagKMPACKEDARGB, *P_tagKMPACKEDARGB;

typedef union _tagKMPACKEDARGB KMPACKEDARGB;

typedef struct _struct_92 _struct_92, *P_struct_92;

typedef uchar KMBYTE;

struct _struct_92 {
    KMBYTE bBlue;
    KMBYTE bGreen;
    KMBYTE bRed;
    KMBYTE bAlpha;
};

union _tagKMPACKEDARGB {
    KMDWORD dwPacked;
    struct _struct_92 byte;
};

typedef enum enum_65 {
    KM_DSPMODE_VGA320x240=0,
    KM_DSPMODE_VGA320x480=1,
    KM_DSPMODE_VGA640x240=2,
    KM_DSPMODE_VGA640x480=3,
    KM_DSPMODE_NTSCNI320x240=4,
    KM_DSPMODE_NTSCI320x240=5,
    KM_DSPMODE_NTSCNI640x240=6,
    KM_DSPMODE_NTSCI640x240=7,
    KM_DSPMODE_NTSCNI320x480FF=8,
    KM_DSPMODE_NTSCNI320x480=9,
    KM_DSPMODE_NTSCI320x480=10,
    KM_DSPMODE_NTSCNI640x480FF=11,
    KM_DSPMODE_NTSCNI640x480=12,
    KM_DSPMODE_NTSCI640x480=13,
    KM_DSPMODE_PALNI320x240=14,
    KM_DSPMODE_PALI320x240=15,
    KM_DSPMODE_PALNI640x240=16,
    KM_DSPMODE_PALNI640x240EXT=17,
    KM_DSPMODE_PALI640x240=18,
    KM_DSPMODE_PALNI320x480FF=19,
    KM_DSPMODE_PALNI320x480=20,
    KM_DSPMODE_PALI320x480=21,
    KM_DSPMODE_PALNI640x480FF=22,
    KM_DSPMODE_PALNI640x480=23,
    KM_DSPMODE_PALI640x480=24,
    KM_DSPMODE_PALNI640x480FFEXT=25,
    KM_DSPMODE_PALNI640x480EXT=26,
    KM_DSPMODE_PALI640x480EXT=27
} enum_65;

typedef enum enum_65 KMDISPLAYMODE;

typedef enum enum_66 {
    KM_DSPBPP_RGB565=0,
    KM_DSPBPP_RGB555=1,
    KM_DSPBPP_ARGB1555=3,
    KM_DSPBPP_RGB888=4,
    KM_DSPBPP_ARGB8888=5
} enum_66;

typedef enum enum_66 KMBPPMODE;

typedef void VOID;

typedef VOID KMVOID;

typedef KMVOID *PKMVOID;

typedef VOID (*PKMCALLBACKFUNC)(PKMVOID);

typedef KMFLOAT *PKMFLOAT;

typedef struct _tagKMRECT _tagKMRECT, *P_tagKMRECT;

typedef struct _tagKMRECT *PKMRECT;

struct _tagKMRECT {
    KMINT32 nXmin;
    KMINT32 nYmin;
    KMINT32 nXmax;
    KMINT32 nYmax;
};

typedef enum _tagKM_VIRTICAL_FILETERMODE {
    KM_VIRTICAL_FILTER_NO_FILTER=0,
    KM_VIRTICAL_FILTER_NORMAL=1,
    KM_VIRTICAL_FILTER_SQUARE_PIXEL=2,
    KM_VIRTICAL_FILTER_VGA=3
} _tagKM_VIRTICAL_FILETERMODE;

typedef enum _tagKM_VIRTICAL_FILETERMODE KM_VIRTICAL_FILETERMODE;

typedef int Int;

typedef float Float;

typedef Float NJS_MATRIX[16];

typedef struct NJS_POINT3 NJS_POINT3, *PNJS_POINT3;

struct NJS_POINT3 {
    Float x;
    Float y;
    Float z;
};

typedef struct NJS_CNK_MODEL NJS_CNK_MODEL, *PNJS_CNK_MODEL;

struct NJS_POINT3 {
    Float x;
    Float y;
    Float z;
};

struct NJS_CNK_MODEL {
    Sint32 *vlist;
    Sint16 *plist;
    struct NJS_POINT3 center;
    Float r;
};

typedef struct NJS_PVRHEADERDATA NJS_PVRHEADERDATA, *PNJS_PVRHEADERDATA;

struct NJS_PVRHEADERDATA {
    Uint32 nTextureType;
    Uint16 nWidth;
    Uint16 nHeight;
};

typedef struct NJS_POINT3 NJS_VECTOR;

typedef struct NJS_VERTEXBUFFER_INFO NJS_VERTEXBUFFER_INFO, *PNJS_VERTEXBUFFER_INFO;

typedef struct NJS_PASS_INFO NJS_PASS_INFO, *PNJS_PASS_INFO;

struct NJS_VERTEXBUFFER_INFO {
    void *pVertexBuffer;
    Uint32 nVertexBufferSize;
    Uint32 nTextureMemorySize;
    Bool bNoWaitVsync;
    Sint32 nNumOfMaxPass;
    Sint32 nNumOfTextures;
    Sint32 nNumOfSmallVQ;
    struct NJS_PASS_INFO sPassInfo[8];
};

struct NJS_PASS_INFO {
    Bool bPreSort;
    Sint32 nSize[5];
};

typedef struct cnkobj cnkobj, *Pcnkobj;

typedef struct cnkobj NJS_CNK_OBJECT;

typedef Sint32 Angle;

struct cnkobj {
    Uint32 evalflags;
    struct NJS_CNK_MODEL *model;
    Float pos[3];
    Angle ang[3];
    Float scl[3];
    struct cnkobj *child;
    struct cnkobj *sibling;
    Float re_quat;
};

typedef struct NJS_TEXLIST NJS_TEXLIST, *PNJS_TEXLIST;

typedef struct NJS_TEXNAME NJS_TEXNAME, *PNJS_TEXNAME;

struct NJS_TEXLIST {
    struct NJS_TEXNAME *textures;
    Uint32 nbTexture;
};

struct NJS_TEXNAME {
    void *filename;
    Uint32 attr;
    Uint32 texaddr;
};

typedef struct NJS_MOTION NJS_MOTION, *PNJS_MOTION;

struct NJS_MOTION {
    void *mdata;
    Uint32 nbFrame;
    Uint16 type;
    Uint16 inp_fn;
};

typedef struct NJS_SPHERE NJS_SPHERE, *PNJS_SPHERE;

struct NJS_POINT3 {
    Float x;
    Float y;
    Float z;
};

struct NJS_SPHERE {
    struct NJS_POINT3 c;
    Float r;
};

typedef struct NJS_POINT2 NJS_POINT2, *PNJS_POINT2;

struct NJS_POINT2 {
    Float x;
    Float y;
};

typedef struct NJS_MOTION_LINK NJS_MOTION_LINK, *PNJS_MOTION_LINK;

struct NJS_MOTION_LINK {
    struct NJS_MOTION *motion[2];
    Float frame[2];
};

typedef struct NJS_POLYGON_VTX NJS_POLYGON_VTX, *PNJS_POLYGON_VTX;

struct NJS_POLYGON_VTX {
    Float x;
    Float y;
    Float z;
    Uint32 col;
};

typedef struct NJS_TEXTURE_VTX NJS_TEXTURE_VTX, *PNJS_TEXTURE_VTX;

struct NJS_TEXTURE_VTX {
    Float x;
    Float y;
    Float z;
    Float u;
    Float v;
    Uint32 col;
};

typedef Float NJS_FOG_TABLE[128];

typedef struct NJS_QUATERNION NJS_QUATERNION, *PNJS_QUATERNION;

struct NJS_QUATERNION {
    float re;
    float im[3];
};

typedef struct NJS_TEXMANAGE NJS_TEXMANAGE, *PNJS_TEXMANAGE;

typedef struct NJS_TEXSYSTEM NJS_TEXSYSTEM, *PNJS_TEXSYSTEM;

typedef struct NJS_TEXSURFACE NJS_TEXSURFACE, *PNJS_TEXSURFACE;

struct NJS_TEXMANAGE {
    Uint32 tspparam;
    Uint32 texparam;
    Uint32 bank;
    struct NJS_TEXSYSTEM *texsys;
    Int count;
    Uint32 texflag;
};

struct NJS_TEXSURFACE {
    Uint32 Type;
    Uint32 BitDepth;
    Uint32 PixelFormat;
    Uint32 nWidth;
    Uint32 nHeight;
    Uint32 TextureSize;
    Uint32 fSurfaceFlags;
    Uint32 *pSurface;
};

struct NJS_TEXSYSTEM {
    Uint32 globalIndex;
    struct NJS_TEXSURFACE texsurface;
    Int count;
};

typedef struct NJS_TEXERRSTAT NJS_TEXERRSTAT, *PNJS_TEXERRSTAT;

struct NJS_TEXERRSTAT {
    Uint32 n;
    Uint32 globalIndex;
    Sint32 texerr;
    Sint32 gdstat;
    Sint32 gderr;
    Sint32 reserved0;
    Sint32 reserved1;
    Sint32 reserved2;
};

typedef struct NJS_VERTEX_BUF NJS_VERTEX_BUF, *PNJS_VERTEX_BUF;

struct NJS_VERTEX_BUF {
    Float dmy[16];
};

typedef struct NJS_CAMERA NJS_CAMERA, *PNJS_CAMERA;

typedef Sint16 Sangle;

struct NJS_CAMERA {
    Float px;
    Float py;
    Float pz;
    Float vx;
    Float vy;
    Float vz;
    Sangle roll;
    Sangle ang;
    Uint32 type;
};

typedef struct NJS_TEXPALETTE NJS_TEXPALETTE, *PNJS_TEXPALETTE;

struct NJS_TEXPALETTE {
    void *palette;
    Uint16 mode;
    Sint16 bank;
    Sint16 offset;
    Sint16 count;
};

typedef struct NJS_TEXTUREH_VTX NJS_TEXTUREH_VTX, *PNJS_TEXTUREH_VTX;

struct NJS_TEXTUREH_VTX {
    Float x;
    Float y;
    Float z;
    Float u;
    Float v;
    Uint32 bcol;
    Uint32 ocol;
};

typedef struct NJS_ARGB NJS_ARGB, *PNJS_ARGB;

struct NJS_ARGB {
    Float a;
    Float r;
    Float g;
    Float b;
};

typedef struct NJS_CSHAPELIST NJS_CSHAPELIST, *PNJS_CSHAPELIST;

typedef struct NJS_CSHAPE_KEYGROUP NJS_CSHAPE_KEYGROUP, *PNJS_CSHAPE_KEYGROUP;

typedef struct NJS_CSHAPECV NJS_CSHAPECV, *PNJS_CSHAPECV;

struct NJS_CSHAPELIST {
    struct NJS_CSHAPE_KEYGROUP *shapes;
    Uint32 nbShape;
};

struct NJS_CSHAPE_KEYGROUP {
    struct NJS_CSHAPECV *cvlist;
    Uint16 groupId;
    Uint16 reserved;
};

struct NJS_CSHAPECV {
    Sint32 *cv;
};

typedef struct NJS_TEXINFO NJS_TEXINFO, *PNJS_TEXINFO;

struct NJS_TEXSURFACE {
    Uint32 Type;
    Uint32 BitDepth;
    Uint32 PixelFormat;
    Uint32 nWidth;
    Uint32 nHeight;
    Uint32 TextureSize;
    Uint32 fSurfaceFlags;
    Uint32 *pSurface;
};

struct NJS_TEXINFO {
    void *texaddr;
    struct NJS_TEXSURFACE texsurface;
};

typedef struct PDS_PERIPHERAL PDS_PERIPHERAL, *PPDS_PERIPHERAL;

typedef struct PDS_PERIPHERALINFO PDS_PERIPHERALINFO, *PPDS_PERIPHERALINFO;

struct PDS_PERIPHERAL {
    Uint32 id;
    Uint32 support;
    Uint32 on;
    Uint32 off;
    Uint32 press;
    Uint32 release;
    Uint16 r;
    Uint16 l;
    Sint16 x1;
    Sint16 y1;
    Sint16 x2;
    Sint16 y2;
    char *name;
    void *extend;
    Uint32 old;
    struct PDS_PERIPHERALINFO *info;
};

struct PDS_PERIPHERALINFO {
    Uint32 type;
    Uint32 reserved[3];
    Uint8 is_root;
    Uint8 area_code;
    Uint8 connector_dir[2];
    char product_name[32];
    char license[64];
    Uint16 stdby_pow;
    Uint16 max_pow;
};

typedef struct PDS_VIBUNITINFO PDS_VIBUNITINFO, *PPDS_VIBUNITINFO;

typedef Sint32 (*PD_VIBENUMUNITCALLBACK)(Uint32, Sint32, struct PDS_VIBUNITINFO *, Uint32);

struct PDS_VIBUNITINFO {
    Uint8 position;
    Uint8 axis;
    Uint8 pow_enable;
    Uint8 cont_enable;
    Uint8 dir_enable;
    Uint8 wave_enable;
    Uint8 min_freq;
    Uint8 max_freq;
};

typedef struct PDS_VIBINFO PDS_VIBINFO, *PPDS_VIBINFO;

struct PDS_VIBINFO {
    Uint8 units;
    Uint8 se_units;
};

typedef struct PDS_VIBPARAM PDS_VIBPARAM, *PPDS_VIBPARAM;

struct PDS_VIBPARAM {
    Uint8 unit;
    Uint8 flag;
    Sint8 power;
    Uint8 freq;
    Uint8 inc;
    Uint8 reserved[3];
};

typedef enum SDE_ERR {
    SDE_ERR_NOTHING=0,
    SDE_ERR_PRM_OVER_RANGE=50331649,
    SDE_ERR_PRM_ILLEGAL_VALUE=50331650,
    SDE_ERR_MODULE_SLOT_NO_ENOUGH=83886081,
    SDE_ERR_MEMBLK_ALREADY_TRANSFER=100663297,
    SDE_ERR_MEMBLK_QUEUE_NO_ENOUGH=100663553,
    SDE_ERR_HANDLE_NO_ENOUGH=117440769,
    SDE_ERR_HANDLE_ILLEGAL_VALUE=117440770,
    SDE_ERR_HANDLE_NULL=117440771,
    SDE_ERR_PTR_ILLEGAL_VALUE=117441026,
    SDE_ERR_PTR_NULL=117441027,
    SDE_ERR_BANK_ILLEGAL_TYPE=150994945,
    SDE_ERR_BANK_ILLEGAL_VER=150994946,
    SDE_ERR_BANK_NOTHING=150994947,
    SDE_ERR_BANK_ILLEGAL_NUM=150994948,
    SDE_ERR_BANK_ILLEGAL_SZ=150994949,
    SDE_ERR_BANK_ILLEGAL_CHECK_SUM=150994950,
    SDE_ERR_DATA_ILLEGAL_TYPE=150995201,
    SDE_ERR_DATA_ILLEGAL_VER=150995202,
    SDE_ERR_DATA_NOTHING=150995203,
    SDE_ERR_DATA_ILLEGAL_NUM=150995204,
    SDE_ERR_BANK_NO_MAPPING=150995216,
    SDE_ERR_HOST_CMD_BUF_NO_ENOUGH=167772161,
    SDE_ERR_DRV_PROBLEM=184549377,
    SDE_ERR_DRV_ILLEGAL_VER=184549378,
    SDE_ERR_DRV_BUSY=184549379,
    SDE_ERR_NO_INIT=218103809,
    SDE_ERR_ALREADY_INIT=218103810,
    SDE_ERR_HARD_WARE=251658241,
    SDE_ERR_MAIN_MEM_ADR_ERR=251723777,
    SDE_ERR_SND_MEM_ADR_ERR=251724033,
    SDE_ERR_UNKNOW_NUM=2147483647
} SDE_ERR;

typedef struct SDS_MEMBLK SDS_MEMBLK, *PSDS_MEMBLK;

typedef struct SDS_MEMBLK *SDMEMBLK;

struct SDS_MEMBLK {
    Uint32 m_Member[16];
};

typedef enum SDE_DATA_TYPE {
    SDE_DATA_TYPE_DRV=0,
    SDE_DATA_TYPE_MLT=1,
    SDE_DATA_TYPE_MIDI_DRM_BANK=2,
    SDE_DATA_TYPE_MIDI_SEQ_BANK=3,
    SDE_DATA_TYPE_MIDI_PRG_BANK=4,
    SDE_DATA_TYPE_SHOT_BANK=5,
    SDE_DATA_TYPE_PSTM_RING_BUF=6,
    SDE_DATA_TYPE_FX_OUT_BANK=7,
    SDE_DATA_TYPE_FX_PRG_BANK=8,
    SDE_DATA_TYPE_FX_PRG_WRK=9
} SDE_DATA_TYPE;

typedef enum SDE_MEMBLK_STAT {
    SDE_MEMBLK_STAT_TRANSFER_REMAIN=0,
    SDE_MEMBLK_STAT_TRANSFER_FINISHED=1,
    SDE_MEMBLK_STAT_TRANSFER_PROGRESS=2
} SDE_MEMBLK_STAT;

typedef void Void;

typedef Void (*SD_MEMBLK_CALLBACK_FUNC)(Void *);

typedef enum SDE_MEMBLK_TRANSFER_MODE {
    SDE_MEMBLK_TRANSFER_MODE_CPU=0,
    SDE_MEMBLK_TRANSFER_MODE_DMA=1
} SDE_MEMBLK_TRANSFER_MODE;

typedef struct SDS_PORT_REF SDS_PORT_REF, *PSDS_PORT_REF;

typedef struct SDS_PORT_REF *SDHANDLE;

typedef SDHANDLE SDMIDI;

struct SDS_PORT_REF {
    Void *m_Ref;
};

typedef struct SDS_MIDI_STAT SDS_MIDI_STAT, *PSDS_MIDI_STAT;

struct SDS_MIDI_STAT {
    Sint16 rsv;
    Sint8 m_Vol;
    Sint8 m_Pan;
    Sint8 m_FxInLev;
    Sint8 m_DrctLev;
    Sint16 m_Pitch;
    Sint16 m_SeqSpeed;
    Uint32 m_SeqTotalBeatTime;
    Sint32 m_SeqCurAdr;
    Uint32 m_Err;
    Uint32 m_Flg;
};

typedef SDHANDLE SDPSTM;

typedef enum SDE_PCM_TYPE {
    SDE_PCM_TYPE_8BIT_LINEAR=0,
    SDE_PCM_TYPE_16BIT_LINEAR=1,
    SDE_PCM_TYPE_4BIT_ADPCM=2
} SDE_PCM_TYPE;

typedef SDHANDLE SDSHOT;

typedef struct SDS_SHOT_STAT SDS_SHOT_STAT, *PSDS_SHOT_STAT;

struct SDS_SHOT_STAT {
    Sint16 rsv;
    Sint8 m_Vol;
    Sint8 m_Pan;
    Sint8 m_FxInLev;
    Sint8 m_DrctLev;
    Sint16 m_Pitch;
    Sint16 rsv2;
    Uint32 m_TotalSmpFrame;
    Sint32 m_CurAdr;
    Uint32 m_Err;
    Uint32 m_Flg;
};

typedef enum SDE_PAN_MODE {
    SDE_PAN_MODE_MONO=0,
    SDE_PAN_MODE_STEREO=1
} SDE_PAN_MODE;

typedef enum SDE_HOST_CMD_FLUSH_MODE {
    SDE_HOST_CMD_FLUSH_MODE_NORMAL=0,
    SDE_HOST_CMD_FLUSH_MODE_FORCE=1,
    SDE_HOST_CMD_FLUSH_MODE_FORCE_ALL=2
} SDE_HOST_CMD_FLUSH_MODE;

typedef Void (*SD_ERR_CALLBACK_FUNC)(Void *, enum SDE_ERR, char *);

typedef enum enum_17 {
    SYE_BT_FNT_FONTSET_WESTERN_24=0,
    SYE_BT_FNT_FONTSET_JP_JIS_24=1,
    SYE_BT_FNT_FONTSET_JP_KANA_24=2,
    SYE_BT_FNT_FONTSET_JP_LVL1_24=3,
    SYE_BT_FNT_FONTSET_JP_LVL2_24=4,
    SYE_BT_FNT_FONTSET_JP_GAIJ_24=5,
    SYE_BT_FNT_FONTSET_VMSICON=6,
    SYE_BT_FNT_FONTSET_END=7,
    SYE_BT_FNT_FONTSET_JP_ASCII_12=8,
    SYE_BT_FNT_FONTSET_JP_ASCII_JP_12=9
} enum_17;

typedef enum enum_17 SYE_BT_FNT_FONTSET;

typedef struct tag_SYS_BT_FNT_INFO tag_SYS_BT_FNT_INFO, *Ptag_SYS_BT_FNT_INFO;

typedef struct tag_SYS_BT_FNT_INFO SYS_BT_FNT_INFO;

struct tag_SYS_BT_FNT_INFO {
    Uint8 width;
    Uint8 height;
};

typedef Uint32 SY_CACHE_FORM;

typedef Uint32 SYCALLBACK;

typedef Uint32 SY_CALLBACK_EVENT;

typedef struct SyFbrS_INFO_tag SyFbrS_INFO_tag, *PSyFbrS_INFO_tag;

typedef struct SyFbrS_INFO_tag SYS_FBR_INFO;

typedef SYS_FBR_INFO *SYFBR;

struct SyFbrS_INFO_tag {
    Void *sp;
    Void *data;
};

typedef Void (*SyFbrYfunc)(Void *);

typedef struct SYS_MALLOC_IF SYS_MALLOC_IF, *PSYS_MALLOC_IF;

struct SYS_MALLOC_IF {
    Void * (*malloc)(Uint32);
    Void (*free)(Void *);
    Void * (*calloc)(Uint32, Uint32);
    Void * (*realloc)(Void *, Uint32);
};

typedef struct _SYS_RTC_DATE _SYS_RTC_DATE, *P_SYS_RTC_DATE;

typedef struct _SYS_RTC_DATE SYS_RTC_DATE;

struct _SYS_RTC_DATE {
    Uint16 year;
    Uint8 month;
    Uint8 day;
    Uint8 hour;
    Uint8 minute;
    Uint8 second;
    Uint8 dayofweek;
    Uint8 ageofmoon;
};

typedef Uint16 SY_TMR_CLOCK;


Sint32 buAnalyzeBackupFileImage(BUS_BACKUPFILEHEADER * hdr, void * buf);
Sint32 buCalcBackupFileSize(Uint32 inum, Uint32 vtype, Uint32 size);
Uint16 buCalcCRC(void * buf, Uint32 size);
Sint32 buDefragDisk(Sint32 drive, void * work);
Sint32 buDeleteFile(Sint32 drive, char * fname);
Sint32 buDeleteFileEx(Sint32 drive, char * * fname, Sint32 nfiles);
void buExecServer(void);
Sint32 buExit(void);
Sint32 buFormatDisk(Sint32 drive, Uint8 * volume, Sint32 icon, BUS_TIME * time, Uint32 flag);
Sint32 buGetFileSize(Sint32 drive, char * fname);
Sint32 buGetLastError(Sint32 drive);
Uint32 buGetVersion(void);
Sint32 buInit(Sint32 capacity, Sint32 drives, void * work, BU_INITCALLBACK func);
Sint32 buIsFormat(Sint32 drive);
Sint32 buIsReady(Sint32 drive);
Sint32 buLoadFile(Sint32 drive, char * fname, void * buf, Uint32 nblock);
Sint32 buLoadFileEx(Sint32 drive, char * fname, void * buf, Uint32 start, Uint32 nblock);
Sint32 buMakeBackupFileImage(void * buf, BUS_BACKUPFILEHEADER * hdr);
Sint32 buMount(Sint32 drive);
Sint32 buMountDisk(Sint32 drive, void * addr, Sint32 size);
Sint32 buRemount(Sint32 drive);
Sint32 buRenameFile(Sint32 drive, char * from, char * to);
Sint32 buRewriteExecFile(Sint32 drive, char * fname, void * buf, Uint32 start, Uint32 nblock);
Sint32 buRewriteFile(Sint32 drive, char * fname, void * buf, Uint32 start, Uint32 nblock);
Sint32 buSaveExecFile(Sint32 drive, char * fname, void * buf, Uint32 nblock, BUS_TIME * time, Uint32 flag);
Sint32 buSaveFile(Sint32 drive, char * fname, void * buf, Uint32 nblock, BUS_TIME * time, Uint32 flag);
void buSetCallbackParam(Uint32 param);
void buSetCompleteCallback(BU_COMPLETECALLBACK func);
Sint32 buSetFileAttr(Sint32 drive, char * fname, Uint16 hdrofs, Uint8 copyflag);
void buSetPrintFunc(void (*func)(char * str));
Sint32 buStat(Sint32 drive);
Sint32 buUnmount(Sint32 drive);
Sint32 buUnmountDisk(Sint32 drive);
void * calloc(size_t, size_t);
void free(void *);
double frexp(double, int *);
Sint32 gdFsChangeDir(char * dirname);
Sint32 gdFsCheckEof(GDFS gdfs, Bool * iseof);
void gdFsClose(GDFS gdfs);
GDFS_DIRREC gdFsCreateDirhn(void * dirbuf, Sint32 max_dirent);
Sint32 gdFsDaGetInfo(GDFS_DAINFO * dainfo);
Sint32 gdFsDaPause(void);
Sint32 gdFsDaPlay(Sint32 st_track, Sint32 end_track, Sint32 reptime);
Sint32 gdFsDaPlaySct(Sint32 st_sct, Sint32 end_sct, Sint32 reptime);
Sint32 gdFsDaRelease(Sint32 reptime);
Sint32 gdFsDaStop(void);
void gdFsEntryErrFunc(GDFS gdfs, GDFS_ERRFUNC erfunc, void * obj);
void gdFsEntryErrFuncAll(GDFS_ERRFUNC erfunc, void * obj);
void gdFsEntryRdEndFunc(GDFS gdfs, GDFS_FUNC func, void * obj);
void gdFsEntryTrEndFunc(GDFS gdfs, GDFS_FUNC func, void * obj);
void gdFsExecServer(void);
void gdFsFinish(void);
Sint32 gdFsGetDirInfo(char * name, GDFS_DIRINFO * dirinfo);
Sint32 gdFsGetDrvStat(void);
Sint32 gdFsGetErrStat(GDFS gdfs);
Bool gdFsGetFileFad(GDFS gdfs, Sint32 * fad);
Bool gdFsGetFileSctSize(GDFS gdfs, Sint32 * fsctsize);
Bool gdFsGetFileSize(GDFS gdfs, Sint32 * fsize);
Sint32 gdFsGetNumRd(GDFS gdfs);
Sint32 gdFsGetStat(GDFS gdfs);
GDFS gdFsGetSysHn(void);
Sint32 gdFsGetToc(Sint32 type, void * buf);
Sint32 gdFsGetTransStat(GDFS gdfs);
GDFS gdFsGetWorkHn(void);
void gdFsInit2(Sint32 max_open, void * gdfs_work, Sint32 max_dirent, void * dirbuf);
Bool gdFsIsTrayOpen(void);
Sint32 gdFsLoadDir(char * dirname, GDFS_DIRREC gf_dirrec);
Sint32 gdFsMount(void);
Sint32 gdFsMovePickup(GDFS gdfs);
GDFS gdFsOpen(char * fname, GDFS_DIRREC gf_dirrec);
GDFS gdFsOpenRange(Sint32 stsct, Sint32 nsct);
Sint32 gdFsReMount(void);
Sint32 gdFsRead(GDFS gdfs, Sint32 nsct, void * buf);
Sint32 gdFsReqDrvStat(void);
Sint32 gdFsReqGdRd(GDFS gdfs, Sint32 nsct);
Sint32 gdFsReqRd32(GDFS gdfs, Sint32 nsct, void * buf);
Sint32 gdFsSeek(GDFS gdfs, Sint32 sctno, Sint32 type);
Sint32 gdFsSetDir(GDFS_DIRREC gf_dirrec);
Sint32 gdFsStopRd(GDFS gdfs);
Sint32 gdFsTell(GDFS gdfs);
Sint32 gdFsTrans32(GDFS gdfs, Sint32 nbytes, void * buf);
KMSTATUS kmAdjustDisplayCenter(KMINT32 nXAdjust, KMINT32 nYAdjust);
KMSTATUS kmBeginPass(PKMVERTEXBUFFDESC pVertexBuffDesc);
KMSTATUS kmBeginScene(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct);
KMSTATUS kmBlankScreen(KMBOOLEAN bBlanking);
KMSTATUS kmChangeDisplayDitherMode(KMBOOLEAN bEnable);
KMSTATUS kmContinuePass(PKMVERTEXBUFFDESC pVertexBuffDesc);
KMSTATUS kmEndPass(PKMVERTEXBUFFDESC pVertexBuffDesc);
KMSTATUS kmEndScene(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct);
KMSTATUS kmFreeTexture(PKMSURFACEDESC pSurfaceDesc);
KMSTATUS kmGarbageCollectTexture(void);
KMSTATUS kmGetCurrentScanline(PKMINT32 pScanline);
KMSTATUS kmGetFreeTextureMem(PKMUINT32 pSizeOfTexture, PKMUINT32 pMaxBlockSizeOfTexture);
KMSTATUS kmGetRenderStatus(KMINT32 nRenderID);
KMSTATUS kmGetSystemMetrics(PKMSYSTEMMETRICS pSysMetrics);
KMSTATUS kmInitDevice(KMDEVICE nDevice);
KMSTATUS kmLoadRectangleTexturePart(PKMSURFACEDESC pSurfaceDesc, PKMDWORD pTexture, KMUINT32 width, KMUINT32 height, KMUINT32 dst_X, KMUINT32 dst_Y, KMUINT32 src_X, KMUINT32 src_Y, KMUINT32 src_U);
KMSTATUS kmLoadVQCodebook(PKMSURFACEDESC pSurfaceDesc, PKMDWORD pTexture);
KMSTATUS kmQueryFinishLastTextureDMA(void);
KMINT32 kmRender(KMDWORD dwFlipMode);
KMSTATUS kmResetRenderer(void);
KMSTATUS kmSetBackGround(PKMSTRIPHEAD pStripHead, KMVERTEXTYPE VertexType, PVOID pVertex1, PVOID pVertex2, PVOID pVertex3);
KMSTATUS kmSetBackGroundPlane(PVOID pVertex[3], KMVERTEXTYPE VertexType, KMINT32 StructSize);
KMSTATUS kmSetBackGroundRenderState(PKMVERTEXCONTEXT pVertexContext);
KMSTATUS kmSetBorderColor(KMPACKEDARGB BorderColor);
KMSTATUS kmSetCheapShadowMode(KMINT32 nIntensity);
KMSTATUS kmSetColorClampValue(KMPACKEDARGB MaxVal, KMPACKEDARGB MinVal);
KMSTATUS kmSetCullingRegister(KMFLOAT fCullVal);
KMSTATUS kmSetDisplayMode(KMDISPLAYMODE nDisplayMode, KMBPPMODE nBpp, KMBOOLEAN fDither, KMBOOLEAN fAntiAlias);
KMSTATUS kmSetEORCallback(PKMCALLBACKFUNC pEORCallback, PVOID pCallbackArguments);
KMSTATUS kmSetEndOfVertexCallback(PKMCALLBACKFUNC pEndOfVertexCallback, PKMVOID pCallbackArguments);
KMSTATUS kmSetFogDensity(KMDWORD fogDensity);
KMSTATUS kmSetFogTable(PKMFLOAT pfFogTable);
KMSTATUS kmSetFogTableColor(KMPACKEDARGB FogTableColor);
KMSTATUS kmSetHSyncCallback(PKMCALLBACKFUNC pHSyncCallback, PVOID pCallbackArguments);
KMSTATUS kmSetHSyncLine(KMINT32 nInterruptLine);
KMSTATUS kmSetPALEXTCallback(PKMCALLBACKFUNC pPalExtCallback, PVOID pCallbackArguments);
KMSTATUS kmSetPunchThroughThreshold(KMDWORD dwThreshold);
KMSTATUS kmSetSystemConfiguration(PKMSYSTEMCONFIGSTRUCT pSystemConfigStruct);
KMSTATUS kmSetUserClipping(PKMVERTEXBUFFDESC pVertexBuffDesc, KMUSERCLIPMODE nPrevUserClipMode, KMLISTTYPE nListType, PKMRECT pRect);
KMSTATUS kmSetVirticalFilterMode(KM_VIRTICAL_FILETERMODE nMode);
KMSTATUS kmSetWaitVsyncCount(KMINT32 nCount);
KMSTATUS kmSetYCoefficient(KMINT32 nYCoeff0, KMINT32 nYCoeff1);
KMSTATUS kmUnloadDevice(void);
KMSTATUS kmWaitVBlank(void);
double log10(double);
void * malloc(size_t);
void njAccumlationBufferMode(Int SrcSelect, Int DstSelect);
void njCalcPoint(NJS_MATRIX * m, NJS_POINT3 * ps, NJS_POINT3 * pd);
void njCalcPointEx(NJS_POINT3 * ps, NJS_POINT3 * pd);
int njCalcScreen(NJS_POINT3 * p, Float * sx, Float * sy);
void njCalcShapeVlist(NJS_CNK_MODEL * cnkmodel);
Uint32 njCalcTexture(Uint32 type);
Uint32 njCalcTextureSize(Uint32 nWidth, Uint32 nHeight, Uint32 nTextureType);
Uint32 njCalcTextureSize2(NJS_PVRHEADERDATA * pvrh);
void njCalcVector(NJS_MATRIX * m, NJS_VECTOR * vs, NJS_VECTOR * vd);
Uint32 njCalcVertexBufferSize(NJS_VERTEXBUFFER_INFO * pVBInfo);
void njCameraExLookAt(NJS_POINT3 * eye, NJS_POINT3 * center, NJS_VECTOR * up);
void njChangeMotionObject(NJS_CNK_OBJECT * cnkobj);
Sint32 njCheckAttrGlobalIndex(NJS_TEXLIST * texlist, Uint32 n, Uint32 * globalIndex, Uint32 * bank);
void njCheckFileName(char * namedst, char * namesrc, char * addext);
Sint32 njCheckPaletteType(Uint32 nTextureType);
Sint32 njCheckTextureFormatManageNum(Sint32 texmng_no, NJS_PVRHEADERDATA * pvrh);
void njClipZ(Float n, Float f);
void njCnkDrawShapeMotion(NJS_CNK_OBJECT * object, NJS_MOTION * motion, NJS_MOTION * shape, Float frame, Float sframe, Sint32 (*pDrawFunc)(NJS_CNK_MODEL * cnkmodel));
void njCnkEasyDrawObject(NJS_CNK_OBJECT * object);
Sint32 njCnkEasyMultiDrawModel(NJS_CNK_MODEL * model);
void njCnkEasyMultiDrawObject(NJS_CNK_OBJECT * model);
void njCnkEasyMultiDrawShapeMotion(NJS_CNK_OBJECT * object, NJS_MOTION * motion, NJS_MOTION * shape, Float frame);
void njCnkSetSimpleLightColor(Float r, Float g, Float b);
Sint32 njCnkSimpleMultiDrawModel(NJS_CNK_MODEL * model);
void njCnkSimpleMultiDrawObject(NJS_CNK_OBJECT * model);
Sint32 njCollisionCheckSS(NJS_SPHERE * sphere1, NJS_SPHERE * sphere2);
void njColorBlendingMode(Int target, Int mode);
void njColorClampMode(Int mode);
void njContinue(void);
void njControl3D(Uint32 flag);
Float njCos(Angle n);
Float njDetMatrix(NJS_MATRIX * m);
void njDrawLine2DExSetList(NJS_POINT2 * vtx, Float ooz, Sint32 Count);
void njDrawLine2DExSetStrip(NJS_POINT2 * vtx, Float ooz, Sint32 Count);
void njDrawLine3DExSetList(NJS_POINT3 * vtx, Sint32 Count);
void njDrawLine3DExSetStrip(NJS_POINT3 * vtx, Sint32 Count);
void njDrawLineExEnd(void);
void njDrawLineExStart(Float r, Uint32 BaseColor, Sint32 Trans);
void njDrawMotionLink(NJS_CNK_OBJECT * object, NJS_MOTION_LINK * motion_link, Float rate, Sint32 (*pDrawFunc)(NJS_CNK_MODEL * cnkmodel));
void njDrawPolygon(NJS_POLYGON_VTX * polygon, Int count, Int trans);
void njDrawPolygon2DExEnd(void);
void njDrawPolygon2DExSetData(NJS_POLYGON_VTX * p, Int count);
void njDrawPolygon2DExStart(Int trans);
void njDrawPolygon3DEx(NJS_POLYGON_VTX * p, Int count, Int trans);
void njDrawPolygon3DExEnd(void);
void njDrawPolygon3DExSetData(NJS_POLYGON_VTX * p, Int count);
void njDrawPolygon3DExStart(Int trans);
void njDrawTexture2DExEnd(void);
void njDrawTexture2DExSetData(NJS_TEXTURE_VTX * p, Int count);
void njDrawTexture2DExStart(Int trans);
void njDrawTexture3DEx(NJS_TEXTURE_VTX * p, Int count, Int trans);
void njDrawTexture3DExEnd(void);
void njDrawTexture3DExSetData(NJS_TEXTURE_VTX * p, Int count);
void njDrawTexture3DExStart(Int trans);
Sint32 njEntryTextureManageNum(NJS_TEXLIST * texlist, Sint32 n, Uint32 globalIndex, Uint32 bank, NJS_PVRHEADERDATA * pvrh, Sint32 * texmng_no);
void njExecuteFade(Float f);
void njExitSystem(void);
void njExitTexture(void);
void njFadeDisable(void);
void njFadeEnable(void);
void njFogDisable(void);
void njFogEnable(void);
void njGarbageTextureEx(void);
void njGenerateFogTable3(NJS_FOG_TABLE fog, Float n, Float f);
void njGetCameraExPosition(NJS_POINT3 * pos);
NJS_TEXLIST * njGetCurrentTexList(void);
void njGetMatrix(NJS_MATRIX * m);
Sint32 njGetMotionEventFlag(void);
int njGetMotionNodeData(NJS_CNK_OBJECT * cnkobj, Float pos[3], Angle ang[3], Float scl[3], NJS_QUATERNION * qua);
void njGetPaletteData(Sint32 offset, Sint32 count, void * data);
Uint32 njGetPaletteMode(void);
void njGetShapeGroupID(void);
Sint32 njGetTexManageNum(NJS_TEXMANAGE * * pmng, NJS_TEXSYSTEM * * psys);
Sint32 njGetTexSurfaceEx(NJS_TEXMANAGE * texmanage, NJS_PVRHEADERDATA * pvrh);
NJS_TEXERRSTAT * njGetTextureErrStat(void);
void njGetTranslation(NJS_MATRIX * m, NJS_POINT3 * p);
void njIgnoreTextureAlphaMode(Int mode);
void njInit3D(NJS_VERTEX_BUF * vbuf, Int vn);
void njInitCamera(NJS_CAMERA * camera);
void njInitMatrix(NJS_MATRIX * pBuf, Uint32 nSize);
void njInitShape(void * buf, Sint32 size);
void njInitTextureErrStat(void);
void njInitTextureEx(NJS_TEXMANAGE * pmng, Int nmng, NJS_TEXSYSTEM * psys, Int nsys);
void njInitTextureGlobalIndex(Uint32 globalIndex);
void njInitVertexBufferEx(NJS_VERTEXBUFFER_INFO * pVBInfo);
void njInterpolateQuaternion(NJS_QUATERNION * qua0, NJS_QUATERNION * qua1, Float rate, NJS_QUATERNION * qua);
Bool njInvertMatrix(NJS_MATRIX * m);
Float njInvertSqrt(Float n);
void njLinkTexlist(NJS_TEXLIST * texlist, Sint32 tex_no, Sint32 texmng_no);
Sint32 njLoadPaletteData(void * data, NJS_TEXPALETTE * pl);
Sint8 * njLoadPvrHeader(NJS_PVRHEADERDATA * pvrh, Uint32 * globalIndex, Sint8 * pTexture);
Sint32 njLoadRectangleTexturePartLowEx(NJS_TEXMANAGE * texmanage, Sint8 * pTexture, Uint32 * p);
Sint32 njLoadTextureFinishDMA(void);
Sint32 njLoadTextureManageNum(Sint32 texmng_no, Sint8 * pTexture);
Sint32 njLoadTexturePartLowEx(NJS_TEXMANAGE * texmanage, Sint8 * pTexture, Uint32 offset, Uint32 size);
Sint32 njLoadTexturePartManageNum(Sint32 texmng_no, Sint8 * pTexture, Uint32 offset, Uint32 size);
Sint32 njLoadVQCodebookLowEx(NJS_TEXMANAGE * texmanage, Sint8 * pTexture);
void njMipmapAdjust(Int level);
void njModifierMode(Int mode);
void njMotionTransformEx(NJS_CNK_OBJECT * cnkobj);
void njMultiMatrix(NJS_MATRIX * md, NJS_MATRIX * ms);
void njPolygonCullingMode(Int mode);
Bool njPopMatrixEx(void);
void njPostMultiMatrix(NJS_MATRIX * md, NJS_MATRIX * ms);
Bool njPushMatrix(NJS_MATRIX * m);
Bool njPushMatrixEx(void);
void njQuaternionEx(NJS_QUATERNION * qua);
void njQuaternionEx2(Float re, Float im[3]);
Sint32 njReadTexturePartLowEx(NJS_TEXMANAGE * texmanage, Sint8 * pTexture, Uint32 offset, Uint32 size);
Sint32 njReleaseTexture(NJS_TEXLIST * texlist);
void njReleaseTextureAll(void);
Sint32 njReleaseTextureLowEx(NJS_TEXMANAGE * texmanage);
Sint32 njReleaseTextureManageNum(Sint32 texmng_no);
Sint32 njReleaseTextureNum(Uint32 n);
Sint32 njRender(void);
void njRotateCameraEx(Angle * ang, Sint32 lv);
void njRotateEx(Angle * ang, Sint32 lv);
void njRotateX(NJS_MATRIX * m, Angle ang);
void njRotateXCameraEx(Angle ang);
void njRotateY(NJS_MATRIX * m, Angle ang);
void njRotateYCameraEx(Angle ang);
void njRotateZ(NJS_MATRIX * m, Angle ang);
void njRotateZCameraEx(Angle ang);
void njScale(NJS_MATRIX * m, Float sx, Float sy, Float sz);
void njScaleEx(NJS_VECTOR * v);
Sint32 njSearchTextureManageNum(Uint32 globalIndex, Uint32 bank, Uint32 nTextureType, Sint32 * texmng_no);
void njSetAspect(Float ax, Float ay);
void njSetBackColor(Uint32 col1, Uint32 col2, Uint32 col3);
void njSetBackTexture(NJS_TEXTUREH_VTX * vtx);
void njSetBorderColor(Uint32 color);
void njSetCamera(NJS_CAMERA * camera);
void njSetCameraExPosition(NJS_POINT3 * pos);
void njSetCnkBlendMode(Uint32 attr);
void njSetColorClamp(Uint32 MaxARGB, Uint32 MinARGB);
void njSetConstantAttr(Uint32 and_attr, Uint32 or_attr);
void njSetConstantMaterial(NJS_ARGB * mat);
void njSetCurMotionInfoSlot(int Slot);
void njSetCurShapeInfoSlot(int Slot);
void njSetDepthQueue(Float dnear, Float dfar);
void njSetEnvelopeWeightValue(Float value);
void njSetFadeColor(Uint32 c);
void njSetFogColor(Uint32 c);
void njSetFogDensity(Uint32 density);
void njSetFogTable(NJS_FOG_TABLE fog);
Sint32 njSetHSyncLine(Int line);
void njSetMatrix(NJS_MATRIX * md, NJS_MATRIX * ms);
void njSetMotionCallback(void (*func)(NJS_CNK_OBJECT * obj));
void njSetNextMotionNodeEx(void);
void njSetNextShapeNodeEx(void);
void njSetPaletteBankNum(Uint32 n, Uint32 bank);
void njSetPaletteBankNumG(Uint32 globalIndex, Uint32 bank);
void njSetPaletteData(Sint32 offset, Sint32 count, void * data);
void njSetPaletteMode(Uint32 mode);
void njSetPerspective(Angle ang);
void njSetPunchThroughThreshold(Int nThreshold);
void njSetShapeList(NJS_CSHAPELIST * shape_list);
void njSetShapeVlist(Uint32 * vlist, Uint32 vtype);
Sint32 njSetTexture(NJS_TEXLIST * texlist);
void njSetTextureInfo(NJS_TEXINFO * info, Sint8 * pTexture, Sint32 nTextureType, Sint32 nWidth, Sint32 nHeight);
Sint32 njSetTextureManageNum(Uint32 globalIndex, Uint32 bank, NJS_PVRHEADERDATA * pvrh);
void njSetTextureName(NJS_TEXNAME * texname, void * addr, Uint32 globalIndex, Uint32 attr);
Sint32 njSetTextureNum(Uint32 n);
Sint32 njSetTextureNumG(Uint32 globalIndex);
void njSetTextureParamEx(NJS_TEXMANAGE * pmng);
void njSetYCoefficientEx(Float fFilterRate, Float fBrightness);
Float njSin(Angle n);
void njSpecularMode(Int mode);
Float njSqrt(Float n);
Sint32 njStartCheck(void);
void njStartDraw(void);
void njStartMotionEx(NJS_MOTION * motion, Float frame);
void njStartMotionObj(NJS_MOTION * motion, Float frame);
void njStartMotionObjEx(NJS_MOTION * motion, Float frame);
void njStartShapeEx(NJS_MOTION * shape, Float frame);
void njSuperSampleMode(Int mode);
Float njTan(Angle n);
Sint32 njTexFreeFixedArea(void);
Sint32 njTexMallocFixedArea(Uint32 size, Sint32 * * pSurface);
void njTextureClampMode(Int mode);
void njTextureFilterMode(Int mode);
void njTextureFlipMode(Int mode);
void njTextureShadingMode(Int mode);
void njTranslate(NJS_MATRIX * m, Float x, Float y, Float z);
void njTranslateAbsoluteCameraEx(Float x, Float y, Float z);
void njTranslateCameraEx(Float x, Float y, Float z);
void njTranslateEx(NJS_VECTOR * v);
void njUnitMatrix(NJS_MATRIX * m);
void njUnitRotPortion(NJS_MATRIX * m);
Float njUnitVector(NJS_VECTOR * v);
void njUserClipping(Int mode, NJS_POINT2 * p);
void njZBufferMode(Int mode);
void pdExecPeripheralServer(void);
void pdExecPeripheralServerUser(void);
void pdExitPeripheral(void);
Uint32 pdGetOptimize(void);
PDS_PERIPHERAL * pdGetPeripheral(Uint32 port);
PDS_PERIPHERALINFO * pdGetPeripheralInfo(Uint32 port);
Sint32 pdGunCheck(void);
void pdGunEnter(Uint32 portbit);
Uint32 pdGunGetFlashColor(void);
Sint32 pdGunGetLatchedPort(void);
void pdGunGetPosition(Sint32 * x, Sint32 * y);
void pdGunLeave(void);
void pdGunSetCallback(Sint32 (*func)(void));
void pdGunSetFlashColor(Uint32 color);
void pdGunSetTrigger(Uint32 port);
void pdInitPeripheral(Sint32 logic, void * recvbuf, void * sendbuf);
void pdInitPeripheralEx(Sint32 logic, Sint32 * permap, void * * recvbuf, void * sendbuf, Sint32 num);
Sint32 pdIsPadReset(Uint32 port);
void pdLcdExit(void);
Sint32 pdLcdGetDirection(Uint32 port);
void pdLcdInit(void);
void pdSetOptimize(Uint32 count);
void pdSetThreshold(Uint8 on, Uint8 off);
Sint32 pdVibEnumerateUnit(Uint32 port, PD_VIBENUMUNITCALLBACK func, Uint32 param);
Sint32 pdVibGetDirection(Uint32 port);
Sint32 pdVibGetInfo(Uint32 port, PDS_VIBINFO * info);
Sint32 pdVibIsReady(Uint32 port);
Sint32 pdVibMxIsReady(Uint32 port);
Sint32 pdVibMxSetStopTime(Uint32 port, Uint32 time);
Sint32 pdVibMxStart(Uint32 port, PDS_VIBPARAM * param);
Sint32 pdVibMxStop(Uint32 port);
Sint32 pdVibSetStopTime(Uint32 port, Uint8 * unit, Uint8 * time, Uint32 num);
Sint32 pdVibStart(Uint32 port, PDS_VIBPARAM * param, Uint32 numunits);
Sint32 pdVmsLcdIsReady(Uint32 port);
Sint32 pdVmsLcdWrite(Uint32 port, void * data, Uint32 flag);
Sint32 pdVmsLcdWrite1(Uint32 port, void * data);
void * realloc(void *, size_t);
char * sbGetVersionString(void);
SDE_ERR sdBankDownload(SDMEMBLK handle, SDE_DATA_TYPE bank_type, Sint8 bank_num);
SDE_ERR sdDrvCheckExecute(void);
SDE_ERR sdDrvGetExecCounter(Uint32 * count_num);
SDE_ERR sdDrvInit(SDMEMBLK handle);
SDE_ERR sdMemBlkCheck(SDMEMBLK handle);
SDE_ERR sdMemBlkCreate(SDMEMBLK * handle);
SDE_ERR sdMemBlkDestroy(SDMEMBLK handle);
SDE_ERR sdMemBlkDestroyAll(void);
SDE_ERR sdMemBlkGetStat(SDMEMBLK handle, SDE_MEMBLK_STAT * stat);
SDE_ERR sdMemBlkSetPrm(SDMEMBLK handle, Void * src_blk_ptr, Sint32 src_blk_sz, SD_MEMBLK_CALLBACK_FUNC callback_func, Void * callback_first_arg);
SDE_ERR sdMemBlkSetTransferMode(SDE_MEMBLK_TRANSFER_MODE transfer_mode);
SDE_ERR sdMemBlkTransfer(SDMEMBLK handle, Sint32 dst_adr);
SDE_ERR sdMidiClose(SDMIDI handle);
SDE_ERR sdMidiCloseAll(void);
SDE_ERR sdMidiGetStat(SDMIDI handle, SDS_MIDI_STAT * midi_stat);
SDE_ERR sdMidiOpen(SDMIDI * handle);
SDE_ERR sdMidiResetPrm(SDMIDI handle);
SDE_ERR sdMidiSeqContinue(SDMIDI handle);
SDE_ERR sdMidiSeqGetCurAdr(SDMIDI handle, Sint32 * cur_adr);
SDE_ERR sdMidiSeqGetTotalBeatTime(SDMIDI handle, Uint32 * cur_beat_time);
SDE_ERR sdMidiSeqPause(SDMIDI handle);
SDE_ERR sdMidiSeqPlay(SDMIDI handle, Sint8 bank_num, Sint8 data_num, Sint8 priority);
SDE_ERR sdMidiSeqSetSpeed(SDMIDI handle, Sint16 speed, Sint32 fade_time);
SDE_ERR sdMidiSeqStop(SDMIDI handle);
SDE_ERR sdMidiSeqStopAll(void);
SDE_ERR sdMidiSetFxInLev(SDMIDI handle, Sint8 fx_lev);
SDE_ERR sdPstmClose(SDPSTM handle);
SDE_ERR sdPstmCloseAll(void);
SDE_ERR sdPstmGetCurAdr(SDPSTM handle, Sint8 target_slot, Sint32 * cur_smp_adr);
SDE_ERR sdPstmIsTransferWaveData(SDPSTM handle, Sint8 target_slot, Sint32 data_sz, Bool * flg);
SDE_ERR sdPstmPlay(SDPSTM handle, SDE_PCM_TYPE pcm_type, Uint16 freq);
SDE_ERR sdPstmResetPrm(SDPSTM handle, Sint8 target_slot);
SDE_ERR sdPstmSetPan(SDPSTM handle, Sint8 target_slot, Sint8 pan, Sint32 fade_time);
SDE_ERR sdPstmSetPitch(SDPSTM handle, Sint8 target_slot, Sint16 pitch, Sint32 fade_time);
SDE_ERR sdPstmSetVol(SDPSTM handle, Sint8 target_slot, Sint8 vol, Sint32 fade_time);
SDE_ERR sdPstmStop(SDPSTM handle);
SDE_ERR sdPstmTransferWaveData(SDPSTM handle, Sint8 target_slot, SDMEMBLK memblk);
SDE_ERR sdShotClose(SDSHOT handle);
SDE_ERR sdShotCloseAll(void);
SDE_ERR sdShotGetCurAdr(SDSHOT handle, Sint32 * cur_smp_adr);
SDE_ERR sdShotGetStat(SDSHOT handle, SDS_SHOT_STAT * shot_stat);
SDE_ERR sdShotGetTotalSmpFrame(SDSHOT handle, Uint32 * cur_smp_frame);
SDE_ERR sdShotOpen(SDSHOT * handle);
SDE_ERR sdShotPlay(SDSHOT handle, Sint8 bank_num, Sint8 data_num, Sint8 priority);
SDE_ERR sdShotResetPrm(SDSHOT handle);
SDE_ERR sdShotSetFxInLev(SDSHOT handle, Sint8 fx_lev);
SDE_ERR sdShotStop(SDSHOT handle);
SDE_ERR sdShotStopAll(void);
SDE_ERR sdSndClearFxPrg(void);
SDE_ERR sdSndMemBlockGetMap(SDE_DATA_TYPE bank_type, Sint8 bank_num, Sint32 * allocate_ptr, Sint32 * allocate_sz);
SDE_ERR sdSndSetFxPrg(Sint8 new_fx_prg_num, Sint8 new_fx_out_num);
SDE_ERR sdSndSetMasterVol(Sint8 vol);
SDE_ERR sdSndSetPanMode(SDE_PAN_MODE pan_mode);
SDE_ERR sdSndStopAll(void);
SDE_ERR sdSysFinish(void);
SDE_ERR sdSysHostCmdBufFlush(SDE_HOST_CMD_FLUSH_MODE mode);
SDE_ERR sdSysServer(void);
SDE_ERR sdSysSetErrCallback(SD_ERR_CALLBACK_FUNC callback);
SDE_ERR sdSysSetSlotNum(Sint32 midi_slot_num, Sint32 shot_slot_num, Sint32 pstm_slot_num);
int sprintf(char *, char *, ...);
Sint32 syBtCheckDisc(void);
Void syBtExit(void);
Sint32 syBtFntChkSmph(void);
void syBtFntClrSmph(void);
void * syBtFntGet(Uint32 num);
Uint32 syBtFntGetAddr(SYE_BT_FNT_FONTSET set, Sint16 code);
void syBtFntGetInfo(SYE_BT_FNT_FONTSET set, Sint16 code, SYS_BT_FNT_INFO * info);
Sint16 syBtFntSjis2Jis(Sint16 sjis);
Void syCacheAllocCacheRAM(Void * * address, Uint32 block);
Void syCacheFinish_P2_(void);
Void syCacheFreeCacheRAM(Void * address);
SY_CACHE_FORM syCacheGetForm_P2_(void);
Void syCacheICI_P2_(void);
Void syCacheInit_P2_(SY_CACHE_FORM form);
Void syCacheMOVCAL(Void * address, Uint32 data);
Void syCacheOCBI(Void * address, Uint32 size);
Void syCacheOCBP(Void * address, Uint32 size);
Void syCacheOCI_P2_(void);
Void syCacheOCWB(Void * address, Uint32 size);
Void syCachePREF(Void * address, Uint32 size);
Void syCachePurgeAll(void);
Void syCacheSetForm_P2_(SY_CACHE_FORM form);
SYCALLBACK syCallbackAddHandler(SY_CALLBACK_EVENT event, Void (*usrCallbackFunc)(Void *), Uint8 priority, Void * arg);
Void syCallbackDeleteHandler(SYCALLBACK callbackId);
Void syCallbackFinish(void);
Uint32 syCallbackGetNestCount(void);
Void syCallbackInit(void);
void * syCalloc(Uint32 nobj, Uint32 size);
Sint32 syCfgExit(void);
Sint32 syCfgGetBroadcastType(Sint32 *);
Sint32 syCfgGetCountryCode(Sint32 *);
Sint32 syCfgGetDefaultLanguage(Sint32 *);
Sint32 syCfgGetIndividualID(Sint8 *);
Sint32 syCfgGetLanguage(Sint32 *);
Sint32 syCfgGetSoundMode(Sint32 *);
Sint32 syCfgInit(Void *);
Sint32 syCfgSetSoundMode(Sint32);
Sint32 syExtChkExit(void);
Sint32 syExtChkInit(void);
SYFBR syFbrConvertThreadToFiber(Void *);
SYFBR syFbrCreateFiber(Void *, Uint32, SyFbrYfunc, Void *);
Void syFbrDelayedSwitchToFiber(SYFBR);
Sint32 syFbrDeleteFiber(SYFBR);
Sint32 syFbrExit(void);
SYFBR syFbrGetCurrentFiber(void);
Void * syFbrGetFiberData(void);
SYFBR syFbrGetMainFiber(void);
Sint32 syFbrInit(Void *, Sint32);
SYFBR syFbrInitEx(Void *, Sint32, Void *, Void *);
Sint32 syFbrPreInit(Sint32 *, int);
Void syFbrSetCallback(Void (*)(SYFBR, SYFBR));
Sint32 syFbrSetIntStack(Void *);
Void syFbrSwitchToFiber(SYFBR);
Void syFree(Void * ap);
void syFsFinish(void);
void syFsInit(void);
void syHwFinish(void);
void syHwInit(void * stackptr);
Void * syMalloc(Uint32 nbytes);
SYS_MALLOC_IF * syMallocChangeIF(SYS_MALLOC_IF * mallocIF);
Void syMallocFinish(void);
Void syMallocInit(Void * heap, Uint32 size);
Void syMallocStat(Uint32 * wholeFreeSize, Uint32 * biggestFreeBlockSize);
void syMemCopy(void * dst, void * src, Sint32 num);
void syMemCopy16(void * dst, void * src, Sint32 num);
void syMemCopy2(void * dst, void * src, Sint32 num);
void syMemCopy32(void * dst, void * src, Sint32 num);
void syMemCopy4(void * dst, void * src, Sint32 num);
void syMemCopy8(void * dst, void * src, Sint32 num);
void syMemCopySQ(void * dst, void * src, Sint32 num);
void syMemSet(void * dst, Uint8 dat, Sint32 num);
void syMemSet16(void * dst, Uint32 dat, Sint32 num);
void syMemSet2(void * dst, Uint16 dat, Sint32 num);
void syMemSet32(void * dst, Uint32 dat, Sint32 num);
void syMemSet4(void * dst, Uint32 dat, Sint32 num);
void syMemSet8(void * dst, Uint32 dat, Sint32 num);
void syMemSetSQ(void * dst, Uint32 dat, Sint32 num);
void * syRealloc(void * op, Uint32 nbytes);
Sint32 syRtcCompareDate(SYS_RTC_DATE * date1, SYS_RTC_DATE * date2);
void syRtcDateToCount(SYS_RTC_DATE * date, Uint32 * count);
void syRtcExecServer(void);
void syRtcFinish(void);
Sint32 syRtcInit(void);
Uint32 syTmrCountToMicro(Uint32);
Uint32 syTmrDiffCount(Uint32, Uint32);
void syTmrFinish(void);
void syTmrGenCancelInt(void);
Uint32 syTmrGenCountToMicro(Uint32, SY_TMR_CLOCK);
Uint32 syTmrGenDiffCount(Uint32, Uint32);
Uint32 syTmrGenGetCount(void);
Uint32 syTmrGenMicroToCount(Uint32, SY_TMR_CLOCK);
void syTmrGenSetClock(SY_TMR_CLOCK);
void syTmrGenSetCount(Uint32);
void syTmrGenSetInt(void (*)(void *), void *);
void syTmrGenSetIntCount(Uint32);
void syTmrGenStart(void);
void syTmrGenStop(void);
Uint32 syTmrGetCount(void);
void syTmrInit(void);
Uint32 syTmrMicroToCount(Uint32);
Sint32 syVideoExit(void);
Sint32 syVideoGetBroadcastType(void);
Sint32 syVideoGetCableType(void);
Sint32 syVideoGetVideoMode(void);
Sint32 syVideoInit(void);
void syVideoSetBorderColor(Uint32);
void syVideoWaitVBlankSetBorderColor(Uint32);
int vsprintf(char *, char *, char *);
