/* The game's own types and prototypes, for Ghidra's analysis (tools/ghidra/analyze.sh). The
 * analysis parses this file after the SDK's headers, and gives each function that it declares
 * this signature. Written for this project from the game's code. docs/game.md describes them. */

typedef struct Task Task;

/* A task's update or draw function. It gets the task's data. */
typedef void (*TaskFunction)(void *data);

/* The function called at a task's removal. It gets the task's data and the task. */
typedef void (*TaskDestroy)(void *data, Task *task);

/* A task of the task system, the 0x18 bytes before its data. */
struct Task {
    unsigned char level;    /* 0x80 when made. Removals by level take the tasks up to a level. */
    unsigned char removal;  /* 0 while it lives, else the frames to its removal */
    unsigned short pad;
    TaskFunction update;    /* called at each run of its list, or 0 */
    TaskFunction draw;      /* called at each draw of its list, or 0 */
    TaskDestroy destroy;    /* called at the removal, or 0 */
    Task *prev;
    Task *next;
};

/* One of the 7 task lists. */
typedef struct TaskList {
    Task *head;
    Task *tail;
} TaskList;

void taskInitLists(void);
void *taskCreate(TaskFunction update, TaskFunction draw, int size, int list);
void taskDrawList(int list);
Task *taskRemove(int list, Task *task);
void taskKillLevel(int list, unsigned char level);
void taskRemoveLevel(int list, unsigned char level);
void taskKillUpdate(int list, TaskFunction update);
void taskRunList(int list);

/* An entry of a hit shape, 8 bytes. kind: 0 a sphere at the object's origin with the radius in
 * value, 1 the sphere that value points at, and 0x80 on the last entry. */
typedef struct HitEntry {
    unsigned int kind;
    union {
        int radius;
        NJS_SPHERE *sphere;
    } value;
} HitEntry;

/* A hit shape: its entries, their mean center and the radius around it that holds them all. */
typedef struct HitShape {
    unsigned int count;
    NJS_POINT3 center;
    float radius;
    HitEntry *entries;
} HitShape;

/* What an object hits and is hit on, 0x40 bytes of its data. Each frame, the object's draw files
 * the record in one of the hitLists, and hitPass tests the lists against each other.
 * attribute: the high 4 bits say what the object is, the low 4 bits what it hits. A hits B when
 * A's low bits and B's high bits share a bit. The low 2 bits are also the color, 1 white or 2
 * black, and 0x08 marks player 2's shots and fields.
 * flags: 0x01 a hit scores 10 points for each point of damage, 0x02 takes no damage and sounds
 * armoured, 0x04 takes no damage and changes nothing on the hitter, 0x08 gives no score,
 * 0x10 hits only records with 0x20, 0x40 a hit makes a sound, 0x80 no target for homing lasers,
 * 0x100 filed also out of view. */
typedef struct HitRecord {
    unsigned short flags;
    unsigned char attribute;
    /* This frame: the low bits of what hit it, 0x80 when it hit something that took damage, and
     * the color of that in bits 4 and 5. */
    unsigned char hitBy;
    unsigned char sphereHitting;    /* the entry of shapes[0] that last hit */
    unsigned char sphereHit;        /* the entry of shapes[1] that was last hit */
    unsigned short power;           /* the damage it deals, doubled against the other color */
    unsigned short hp;
    unsigned short maxHp;
    unsigned char slot;             /* its entry in hitSlots, or 0xff */
    unsigned char lock;             /* hits only the record in this slot, or 0xff for any */
    unsigned char unknown_e[2];
    HitShape shapes[2];             /* [0] what it hits with, [1] where it is hit */
} HitRecord;

/* A record filed for this frame's hit tests, with its shapes' spheres where the object was
 * drawn. */
typedef struct HitFiled HitFiled;
struct HitFiled {
    HitRecord *hit;
    unsigned char placed[2];    /* 1 when the shape is there and in view */
    unsigned short pad;
    NJS_SPHERE *bounds0;        /* the sphere around shapes[0], or 0 with one entry */
    NJS_SPHERE *spheres0;
    NJS_SPHERE *bounds1;
    NJS_SPHERE *spheres1;
    HitFiled *next;
};

/* One of the 5 hitLists: 0 the ships, 1 the ships' fields, 2 shots and lasers, 3 enemies,
 * 4 enemy bullets. */
typedef struct HitList {
    HitFiled *head;
    HitFiled *tail;
} HitList;

/* An entry of hitSlots, the registry of objects that homing lasers can aim at, 0x20 bytes. */
typedef struct HitSlot {
    unsigned char used;         /* 1 in use, 2 freed at the next draw, 0 free */
    unsigned char visible;      /* the object was drawn in view this frame */
    unsigned short aimed;       /* the damage of the lasers aimed at it */
    unsigned char unknown_4[8];
    void *object;
    NJS_POINT3 position;        /* where it was drawn */
    HitRecord *hit;
} HitSlot;

/* A player's shot: the data of its task, 100 bytes. */
typedef struct Shot {
    unsigned char player;
    unsigned char unknown_1;
    unsigned char side;         /* 1 for the two side shots, 0 for the one in the middle */
    unsigned char scenery;      /* not 0 once its tip met the scenery */
    unsigned char unknown_4[8];
    NJS_POINT3 position;
    NJS_POINT3 velocity;
    HitRecord hit;
} Shot;

/* A player's homing laser: the data of its task, 0x284 bytes. */
typedef struct Laser {
    unsigned char player;
    unsigned char target;       /* the hitSlots entry it aims at, or 0xff */
    unsigned char unknown_2[10];
    int angle;
    unsigned char unknown_10[4];
    float speed;
    NJS_POINT3 position;
    unsigned char unknown_24[12];
    HitRecord hit;
    unsigned char unknown_70[532];
} Laser;

typedef struct Enemy Enemy;

/* An enemy's behaviour, called each frame. A result other than 0 ends the enemy. */
typedef int (*EnemyFunction)(Enemy *enemy);

/* The common data of an enemy, the first 0x7c bytes of its task's data. Its own data follows, and
 * data points at it. A stage script's record function makes the enemy from the record's data. */
struct Enemy {
    unsigned char offscreen;    /* set by the draw while the model is out of view */
    unsigned char unknown_1[3];
    NJS_POINT3 position;
    NJS_POINT3 velocity;        /* added to the position each frame */
    Angle angle[3];
    HitRecord hit;
    unsigned char unknown_68;
    unsigned char flags;        /* 0x01 no model, 0x02 no glow */
    unsigned char unknown_6a[2];
    float motionFrame;
    void *model;                /* or a function that the destruction calls */
    EnemyFunction behaviour;
    void *data;
};

void taskEnd(void *data);
void scoreAdd(int player, int points);
void chainCount(int player, int color, NJS_POINT3 *position);
void chainBonus(int player, NJS_POINT3 *position);
void hitInit(HitRecord *hit, unsigned char attribute, unsigned short hp, unsigned short power,
             HitEntry *shape0, HitEntry *shape1);
void hitApply(unsigned char hitterBits, int list0, int list1, NJS_SPHERE *contact,
              HitRecord *hitter, HitRecord *target);
void hitTestLists(int hitters, int targets);
int hitBoundsTest(HitFiled *hitter, HitFiled *target);
void hitFile(int list, HitRecord *hit);
int hitShapePlace(unsigned int flags, HitShape *shape, NJS_SPHERE **out);
void hitShapeTransform(HitShape *shape, NJS_SPHERE **out);
void hitListsClear(void);
void hitListsResetHits(void);
void hitPass(void);
void hitSoundRequest(char priority, NJS_POINT3 *position);
void hitStart(void);
void hitSlotsInit(void);
int hitSlotTargetable(int slot);
NJS_POINT3 *hitSlotPosition(unsigned char slot);
void hitSlotAim(int slot, unsigned char attribute);
void hitSlotUnaim(int slot, unsigned char attribute);
int laserTarget(NJS_POINT3 *point, char wide);
int sceneryHit(NJS_POINT3 *point);
void playerShotCreate(int player, unsigned char attribute, NJS_POINT3 *position,
                      NJS_POINT3 *velocity, char side);
void playerShotUpdate(Shot *shot);
void playerLaserCreate(char player, unsigned char attribute, NJS_POINT3 *position, int angle,
                       float speed);
void hitShapeInit(HitShape *shape);
int hitRegister(void *object);
void enemyMove(Enemy *enemy);

/* A player's buttons: the bits held in this frame, and the bits new in this frame. The bits:
 * 0x01 up, 0x02 down, 0x04 left, 0x08 right, 0x10 shot, 0x20 polarity switch, 0x40 release. */
typedef struct PlayerInput {
    unsigned char held;
    unsigned char pressed;
} PlayerInput;

/* A player's ship: the data of its task, 0x134 bytes.
 * state: 0 a new life, 1 the fly-in, 2 in play, 3 hit, 4 out of the game, 5 held back from joining.
 * flags, and forced (set by others, read with flags): 0x01 no steering, 0x02 no buttons, 0x04 hit,
 * 0x08 invulnerable, 0x10 free of the screen's bounds, 0x20 turning, 0x40 no shots. */
typedef struct Player {
    unsigned char forced;
    unsigned char flags;
    unsigned char index;            /* 0 for player 1, 1 for player 2 */
    unsigned char state;
    unsigned char shotWait;         /* frames to the next shot */
    unsigned char firing;           /* 1 from the first shot while the shot button holds */
    unsigned char polarity;         /* 0 white, 1 black */
    unsigned char release;          /* 1: release the energy as homing lasers */
    unsigned char pressed;          /* 1 shot, 2 switch: pressed in the last two frames */
    unsigned char shotPressWait;
    unsigned char switchPressWait;
    unsigned char switchRequest;
    unsigned char switchHeld;       /* 1 from a switch until its button is let go */
    unsigned char unknown_0d[3];
    unsigned char releaseWait;      /* frames after a release without a new one */
    unsigned char unknown_11[7];
    unsigned short invulnerable;    /* frames left after a new life */
    unsigned short dying;           /* frames since the hit */
    unsigned short flyIn;           /* angle of the fly-in, 0 to 0x4000 */
    unsigned short turn;            /* angle of the polarity switch's turn, 0 for none */
    int turnRoll;
    int turnRollFrom;
    int turnRollTo;
    int roll;                       /* from moving left and right */
    unsigned char unknown_30[12];
    float speed;
    float fieldGrowth;              /* grows back after a polarity switch: the field's size */
    unsigned char unknown_44[4];
    NJS_POINT3 muzzle[2];           /* left and right of the ship, for the shots' flashes */
    NJS_POINT3 position;
    float dx;                       /* the move in this frame */
    float dy;
    unsigned char unknown_74[4];
    float pushX;                    /* added to the move in this frame, then 0 */
    float pushY;
    unsigned char unknown_80[12];
    int rotation;                   /* roll and turnRoll */
    unsigned char unknown_90[12];
    HitRecord hit;                  /* the ship, with the polarity's attribute */
    NJS_SPHERE fieldSphere;
    HitEntry fieldEntry;
    HitRecord field;                /* absorbs the bullets of the ship's color */
} Player;

void playerCreate(int index);
void playerUpdate(Player *player);
void playerDraw(Player *player);
void playerMove(Player *player);
void playerReadInput(Player *player);
void playerButtons(Player *player);
void playerSetPolarity(Player *player);
void playerPlaceAtStart(Player *player);
void playerSpendEnergy(int index, unsigned char amount);

/* A stage script's record function. It gets the record's data, and returns an object or 0. */
typedef void *(*ScriptFunction)(void *data);

/* A record of a stage script, 0x4c bytes. A script is an array of records that a task runs
 * (scriptRun), each record once its condition holds. flags:
 * 0x0001 runs once scrollPosition passes value, 0x0002 branches then.
 * 0x0004 runs once the record's frames reach value, 0x0008 branches then.
 * 0x2000 and 0x1000 do the same with the script's frames.
 * 0x0080 runs once the group in bits 4 and 5 is empty.
 * 0x0800 adds the function's result to the group in bits 8 and 9.
 * 0x8000 ends the script. */
typedef struct ScriptRecord {
    unsigned short flags;
    unsigned char next;         /* records on after the record ran */
    unsigned char branch;       /* records on when a condition branches */
    int value;
    ScriptFunction function;    /* called with data when the record runs, or 0 */
    unsigned char data[64];
} ScriptRecord;

/* The data of the task that runs a stage script. */
typedef struct Script {
    unsigned char unknown_0[4];
    int frames;                 /* frames on the current record */
    int total;                  /* frames since the script started */
    ScriptRecord *record;
} Script;

void scriptStart(ScriptRecord *records);
void scriptRun(Script *script);
int scriptStep(Script *script);
void scriptGroupAdd(int group, void *object);
void scriptGroupRemove(int group, int index);
void scriptGroupsInit(void);

/* An enemy's bullet: the data of its task in list 4, 0x68 bytes. */
typedef struct Bullet {
    float x;
    float y;
    float unknown_8;
    float unknown_c;
    NJS_POINT3 velocity;        /* x and y move the bullet each frame */
    float margin;               /* how far past the screen's edge it lives */
    HitRecord hit;              /* the attribute is its color: 0x11 white, 0x22 black */
    unsigned char frame;        /* its picture, 0 to 3, the next one every other frame */
    unsigned char age;
    unsigned char unknown_62[2];
    unsigned int kind;
} Bullet;

Bullet *bulletCreate(unsigned int kind, unsigned char attribute, float *position, NJS_POINT3 *velocity);
void bulletUpdate(Bullet *bullet);
void bulletAbsorb(Bullet *bullet, unsigned int energy);
void playerAbsorb(int player, unsigned int energy);
int nearestPlayerIndex(float *point);
Player *nearestPlayer(float *point);
int aimAtPlayer(int which, float *point);

/* The data of the stage's task, which stageStart makes with the chapter's stage program. */
typedef struct Stage {
    unsigned char unknown_0[4];
    int *program;               /* the next command of the stage program */
    void *path;                 /* the camera's path, which command 1 loads */
    int demoFrames;             /* frames of the attract demo */
} Stage;

void stageUpdate(Stage *stage);
void stageProgramRun(Stage *stage);
void *easeFloat(unsigned char flags, unsigned char *lock, float *target, int frames, float value);
void *easeWord(unsigned char flags, unsigned char *lock, int *target, int value, int frames);
float *scrollStopAt(float position, float deceleration);

/* A point of a camera path, 0x20 bytes: seven values, which the path moves between in a straight
 * line, or on a cubic curve where flags has the value's bit (0x01 for the first). 0x80 in flags
 * marks the last point. */
typedef struct PathPoint {
    unsigned int flags;
    NJS_POINT3 position;
    float angle[3];             /* in the units of an Angle */
    float value;
} PathPoint;

/* A camera path that pathCreate made from its points. */
typedef struct Path {
    unsigned int count;
    unsigned int unknown_4;
    float *distance;            /* the distance along the path at each point */
    PathPoint *points;
    float *slopes;              /* 14 for each point: two for each value */
} Path;

Path *pathCreate(PathPoint *points);
void pathFree(Path *path);
void pathAt(Path *path, NJS_POINT3 *position, int *angle, int *value, float distance);

/* A voice slot of a sound player, 14 bytes: soundSequenceSlots for the 8 MIDI sequence players,
 * soundEffectSlots for the 20 one-shot players. soundServer starts, pauses and ends the sounds. */
typedef struct SoundSlot {
    unsigned char state;        /* 0 free, 1 playing, 2 to start, 3 paused */
    unsigned char started;      /* the sound library reported it playing */
    unsigned char pause;        /* 1: pause it */
    unsigned char unknown_3;
    unsigned short age;         /* frames it played */
    unsigned short id;          /* its bank list's slot in the high byte, its entry in the low */
    unsigned char bank;
    unsigned char number;
    unsigned char priority;
    unsigned char volume;
    unsigned char level;        /* the volume at its position */
    unsigned char pan;
} SoundSlot;

int soundEffect(float *position, unsigned int id);
int soundSlotFind(SoundSlot *slots, int first, int last, unsigned int id, unsigned char priority);
int soundPan(float *position);
int soundLevel(float *position, int volume);

void randomSeed(unsigned int seed);
unsigned int randomNext(void);

/* An entry of a ranking list, 16 bytes. rankings holds 4 tables, which the difficulty picks, of 12
 * lists of 20: ALL and the chapters 1 to 5 of games with one player, then the same with two. */
typedef struct RankEntry {
    unsigned int score;
    unsigned char prototype;    /* 1: played in the prototype mode */
    unsigned char lives;        /* the game's PLAYER STOCK */
    unsigned char extend;       /* the game's EXTEND */
    unsigned char unknown_7;
    unsigned short unknown_8;
    unsigned char date;         /* the year from 2002 in the high 4 bits, the month in the low 4 */
    unsigned char day;
    unsigned char chapter;      /* ALL: the chapter the game ended in from 0, 5 after the ending */
    char name[3];
} RankEntry;

void optionDefaults(void);
void rankingDefaults(void);
void saveReset(void);
int saveCalcSum(void);
int saveImageMake(void);
void saveImageFree(void);
int saveWrite(char force);
int saveWriteDone(void);
int saveRead(void);
int saveReadDone(void);
int optionsApply(void);
int playTimeAdd(void);
void rankingAdd(unsigned int player, char *name);

/* A polygon of the scenery, which polygonContains tests a point against. */
typedef struct Polygon {
    unsigned short flags;       /* bit 0: the test gives the outside */
    unsigned short count;       /* corners */
    float minX, minY, maxX, maxY;
    float *corners;             /* x and y of each corner */
} Polygon;

unsigned int polygonContains(Polygon *polygon, float *origin, float *point);
int saveCheck(void);

/* A bank of graphics, 0x40 bytes: banks holds 16. A sprite's or a model's id names its bank. */
typedef struct Bank {
    char name[32];
    unsigned char state;        /* 0xFF free */
    unsigned char unknown_21[7];
    void *models;               /* 0x14 bytes each: the texture lists at 4 and 8, the object at 12 */
    void *unknown_2c;
    void *sprites;              /* 8 bytes each: the first texture's number, then the frames */
    NJS_TEXLIST texlist;
    void *unknown_3c;
} Bank;

/* An entry of sceneryPolygons, 12 bytes: sceneryAdd adds one, sceneryHit tests them all. */
typedef struct SceneryPolygon {
    float x, y;                 /* the polygon's origin */
    Polygon *polygon;
} SceneryPolygon;

void sceneryAdd(float *origin, Polygon *polygon);

/* An effect's animation, the first 12 bytes of its data. animStart starts it, and animStep runs it
 * at each frame and returns 1 at its end. */
typedef struct Animation {
    int *frames;                /* pairs: a sprite id, then the frames to show it. In the second
                                   word, bit 31 ends the animation and bit 30 starts it again. */
    unsigned int id;            /* the sprite shown */
    unsigned short index;
    short timer;
} Animation;

void animStart(Animation *anim, int *frames);
int animStep(Animation *anim);

/* A particle of effectParticle, 0x2C bytes of task data. */
typedef struct Particle {
    unsigned int flags;         /* the sprite's flags */
    unsigned int id;            /* the sprite */
    float position[3];
    float velocity[3];
    float scale[2];
    float shrink;               /* taken from the scale at each frame */
} Particle;

void particleUpdate(Particle *particle);
void particleDraw(Particle *particle);

/* A sprite effect of effectBurst or effectFollow, 0x44 bytes of task data. */
typedef struct EffectSprite {
    Animation anim;
    float scale[2];
    unsigned int flags;         /* the sprite's flags */
    void *owner;                /* effectFollow: the task it follows, or 0 */
    float *ownerPosition;
    float position[3];
    float velocity[3];          /* effectBurst */
    float depth;
    unsigned char alpha;
    unsigned char unknown_3d;
    unsigned short pad;
    int angle;
} EffectSprite;

void followUpdate(EffectSprite *effect);
void burstUpdate(EffectSprite *effect);
void effectSpriteDraw(EffectSprite *effect);
void effectSpriteDrawDepth(EffectSprite *effect);
void effectSpriteDrawPlain(EffectSprite *effect);

/* The camera, 0xA8 bytes: the data of the camera's task, which stageCamera points at. */
typedef struct Camera {
    unsigned char mode;          /* 1: the draws of list 1 get a flat view (cameraFlat) */
    unsigned char unknown_1[3];
    int flatPerspective;         /* the flat view's perspective */
    int depth;                   /* taken from an object's z when it draws */
    void *flatTask;              /* the camera's task in list 1 */
    int angles[4];               /* the view's angles: x, y, z and a second y */
    float position[3];
    int perspective;
    NJS_MATRIX view;
    float *follow;               /* the point that the orbit turns about, or 0 */
    float offset[3];
    int orbit[4];                /* the orbit's angles, from the camera path */
    float unknown_90;
    float distance;              /* the orbit's distance */
    void *target;                /* a view to move toward: 4 angles, then a position */
    float blend;                 /* how far toward the target, 0 to 1 */
    int unknown_a0;
    int unknown_a4;
} Camera;

void cameraCreate(float *follow);
void cameraUpdate(Camera *camera);
void cameraDraw(Camera *camera);
void cameraOrbit(Camera *camera);
void cameraView(Camera *camera);

/* Text from a printf format at (x, y), in cells of 16 pixels. SHC passes the format and its values
   on the stack, where each call's C has them after r4 to r7. */
void textPrintf(float x, float y, ...);
/* The screen of a failed disc read, with `message` (DISC READ ERROR, FILE READ ERROR), until the
   tray opens. */
void discErrorScreen(char *message);

/* A lens flare's task in list 4: `init` gets the task once, to set the light's angles at 0x14 and
   0x18, and the task's update calls `update` with it at each run. */
void lensFlareCreate(TaskFunction init, TaskFunction update);
