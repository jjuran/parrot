/* embed.c
 *  Copyright: 2001-2003 The Perl Foundation.  All Rights Reserved.
 *  CVS Info
 *     $Id$
 *  Overview:
 *     The Parrot embedding interface.
 *  Data Structure and Algorithms:
 *     See include/parrot/embed.h and docs/embed.pod.
 *  History:
 *     Initial version by Brent Dax on 2002.1.28
 *  Notes:
 *  References:
 */


#include "parrot/parrot.h"
#include "parrot/embed.h"

struct Parrot_Interp *
Parrot_new(void)
{
    /* interpreter.c:make_interpreter builds a new Parrot_Interp. */
    return make_interpreter(NO_FLAGS);
}

extern void Parrot_initialize_core_pmcs(Interp *interp);

void
Parrot_init(struct Parrot_Interp *interpreter)
{

    if (!interpreter->world_inited) {
        /* global_setup.c:init_world sets up some vtable stuff.
         * It must only be called once.
         */

        interpreter->world_inited = 1;
        init_world(interpreter);
    }
}

void
Parrot_set_flag(struct Parrot_Interp *interpreter, Parrot_Interp_flag flag)
{
    /* These two macros (from interpreter.h) do exactly what they look like. */

    Interp_flags_SET(interpreter, flag);
    switch (flag) {
        case PARROT_BOUNDS_FLAG:
        case PARROT_PROFILE_FLAG:
        case PARROT_TRACE_FLAG:
            Interp_core_SET(interpreter, PARROT_SLOW_CORE);
            break;
        default:
            break;
    }
}

void
Parrot_clear_flag(Parrot_Interp interpreter, Parrot_Interp_flag flag)
{
    Interp_flags_CLEAR(interpreter, flag);
}

Parrot_Int
Parrot_test_flag(Parrot_Interp interpreter, Parrot_Interp_flag flag)
{
    return Interp_flags_TEST(interpreter, flag);
}

void
Parrot_set_run_core(struct Parrot_Interp *interpreter, Parrot_Run_core_t core)
{
    Interp_core_SET(interpreter, core);
}

void
Parrot_setwarnings(struct Parrot_Interp *interpreter, Parrot_warnclass wc)
{
    /* Activates the given warnings.  (Macro from warnings.h.) */
    PARROT_WARNINGS_on(interpreter, wc);
}

struct PackFile *
Parrot_readbc(struct Parrot_Interp *interpreter, char *filename)
{
#if PARROT_HAS_HEADER_UNISTD
    off_t program_size, wanted;
#else
    size_t program_size, wanted;
#endif
    char *program_code;
    struct PackFile *pf;
    PMC * io = NULL;
    INTVAL is_mapped = 0;

#ifdef PARROT_HAS_HEADER_SYSSTAT
    struct stat file_stat;
#endif

#ifdef PARROT_HAS_HEADER_SYSMMAN
    int fd = -1;
#endif

    if (filename == NULL || strcmp(filename, "-") == 0) {
        /* read from STDIN */
        io = PIO_STDIN(interpreter);
        /* read 1k at a time */
        program_size = 0;
    }
    else {

#ifdef PARROT_HAS_HEADER_SYSSTAT
        /* if we have stat(), get the actual file size so we can read it
         * in one chunk. */
        if (stat(filename, &file_stat)) {
            PIO_eprintf(interpreter, "Parrot VM: Can't stat %s, code %i.\n",
                    filename, errno);
            return NULL;
        }

#  ifndef PARROT_HAS_BROKEN_ISREG
        /* S_ISREG is strangely broken my lcc/linux install (though it did
	 * once work */
        if (!S_ISREG(file_stat.st_mode)) {
            PIO_eprintf(interpreter, "Parrot VM: %s is not a normal file.\n",
                    filename);
            return NULL;
        }
#  endif /* PARROT_HAS_BROKEN_ISREG */

        program_size = file_stat.st_size;

#else   /* PARROT_HAS_HEADER_SYSSTAT */

        /* otherwise, we will read it 1k at a time */
        program_size = 0;

#endif  /* PARROT_HAS_HEADER_SYSSTAT */

#ifndef PARROT_HAS_HEADER_SYSMMAN
        io = PIO_open(interpreter, NULL, filename, "<");
        if (!io) {
            PIO_eprintf(interpreter, "Parrot VM: Can't open %s, code %i.\n",
                    filename, errno);
            return NULL;
        }

#else   /* PARROT_HAS_HEADER_SYSMMAN */

        /* the file wasn't from stdin, and we have mmap available- use it */
        io = NULL;

#endif  /* PARROT_HAS_HEADER_SYSMMAN */

        interpreter->current_file = string_make(interpreter, filename,
                strlen(filename), NULL, 0, NULL);
    }
#ifdef PARROT_HAS_HEADER_SYSMMAN
again:
#endif
    /* if we've opened a file (or stdin) with PIO, read it in */
    if (io != NULL) {
        size_t chunk_size;
        char *cursor;
        INTVAL read_result;

        chunk_size = program_size > 0 ? program_size : 1024;
        program_code = (char *)malloc(chunk_size);
        wanted = program_size;
        program_size = 0;

        if (NULL == program_code) {
            /* Whoops, out of memory. */

            PIO_eprintf(interpreter,
                    "Parrot VM: Could not allocate buffer to read packfile from PIO.\n");

            return NULL;
        }

        cursor = (char *)program_code;

        while ((read_result =
                PIO_read(interpreter, io, cursor, chunk_size)) > 0) {
            program_size += read_result;
            if (program_size == wanted)
                break;
            chunk_size = 1024;
            program_code =
                realloc(program_code, program_size + chunk_size);

            if (NULL == program_code) {
                PIO_eprintf(interpreter,
                            "Parrot VM: Could not reallocate buffer while reading packfile from PIO.\n");
                return NULL;
            }

            cursor = (char *)program_code + program_size;
        }

        if (read_result < 0) {
            PIO_eprintf(interpreter,
                    "Parrot VM: Problem reading packfile from PIO.\n");
            return NULL;
        }
        PIO_close(interpreter, io);
    }
    else {
        /* if we've gotten here, we opted not to use PIO to read the file.
         * use mmap */

#ifdef PARROT_HAS_HEADER_SYSMMAN

        fd = open(filename, O_RDONLY | O_BINARY);
        if (!fd) {
            PIO_eprintf(interpreter, "Parrot VM: Can't open %s, code %i.\n",
                    filename, errno);
            return NULL;
        }

        program_code =
            mmap(0, program_size, PROT_READ, MAP_SHARED, fd, (off_t)0);

        if (!program_code) {
            Parrot_warn(interpreter, PARROT_WARNINGS_IO_FLAG,
                    "Parrot VM: Can't mmap file %s, code %i.\n",
                    filename, errno);
            /* try again, now with IO reading the file */
            io = PIO_open(interpreter, NULL, filename, "<");
            if (!io) {
                PIO_eprintf(interpreter,
                        "Parrot VM: Can't open %s, code %i.\n",
                        filename, errno);
                return NULL;
            }
            goto again;
        }
        is_mapped = 1;

#else   /* PARROT_HAS_HEADER_SYSMMAN */

        PIO_eprintf(interpreter, "Parrot VM: uncaught error occurred reading "
                    "file or mmap not available.\n");
        return NULL;

#endif  /* PARROT_HAS_HEADER_SYSMMAN */

    }

    /* Now that we have the bytecode, let's unpack it. */

    pf = PackFile_new(is_mapped);

    if (!PackFile_unpack
        (interpreter, pf, (opcode_t *)program_code, program_size)) {
        PIO_eprintf(interpreter, "Parrot VM: Can't unpack packfile %s.\n",
                filename);
        return NULL;
    }

#ifdef PARROT_HAS_HEADER_SYSMMAN

    if (fd >= 0) {
        close(fd);   /* the man page states, it's ok to close a mmaped file */
    }
#else
    mem_sys_free(program_code);
#endif

    return pf;
}

void
Parrot_loadbc(struct Parrot_Interp *interpreter, struct PackFile *pf)
{
    interpreter->code = pf;
}

static void
setup_argv(struct Parrot_Interp *interpreter, int argc, char ** argv)
{
    INTVAL i;
    PMC *userargv;

    if (Interp_flags_TEST(interpreter, PARROT_DEBUG_FLAG)) {
        PIO_eprintf(interpreter,
                "*** Parrot VM: Setting up ARGV array in P5.  Current argc: %d ***\n",
                argc);
    }

    userargv = pmc_new_noinit(interpreter, enum_class_SArray);
    /* immediately anchor pmc to root set */
    interpreter->pmc_reg.registers[5] = userargv;
    VTABLE_set_pmc_keyed_int(interpreter, interpreter->iglobals,
            (INTVAL)IGLOBALS_ARGV_LIST, userargv);
    VTABLE_init(interpreter, userargv);
    VTABLE_set_integer_native(interpreter, userargv, argc);

    for (i = 0; i < argc; i++) {
        /* Run through argv, adding everything to @ARGS. */
        STRING *arg = string_make(interpreter, argv[i], strlen(argv[i]),
                                  0, PObj_external_FLAG, 0);

        if (Interp_flags_TEST(interpreter, PARROT_DEBUG_FLAG)) {
            PIO_eprintf(interpreter, "\t%vd: %s\n", i, argv[i]);
        }

        VTABLE_push_string(interpreter, userargv, arg);
    }
}

/* XXX This allows a command line option to be visible from inside the
 * Parrot core, this is done using the pointers in the interpreter structure,
 * and the subsystems using this arguments must clear them before running.
 */

void
Parrot_setup_opt(struct Parrot_Interp *interpreter, int n, char *argv)
{
    interpreter->string_reg.registers[n] = (STRING *)argv;
}

static int
prof_sort_f(const void *a, const void *b)
{
    const ProfData *pa = (const ProfData *) a;
    const ProfData *pb = (const ProfData *) b;
    if (pa->time < pb->time)
        return 1;
    if (pa->time > pb->time)
        return -1;
    return 0;
}

static void
print_profile(int status, void *p)
{
    Parrot_Interp interpreter = (Parrot_Interp) p;

    if (interpreter->profile != NULL) {
        UINTVAL j;
        int k;
        UINTVAL op_count = 0;
        UINTVAL call_count = 0;
        FLOATVAL sum_time = 0.0;

        PIO_printf(interpreter, "\n\n");
        PIO_printf(interpreter, "                   OPERATION PROFILE                 \n\n");
        PIO_printf(interpreter, "  CODE   OP FULL NAME            CALLS  TOTAL TIME    AVG TIME\n");
        PIO_printf(interpreter, "  -----  -----------------     -------  ----------  ----------\n");

        for (j = 0; j < interpreter->op_count + PARROT_PROF_EXTRA; j++)
            interpreter->profile->data[j].op = j;
        qsort(interpreter->profile->data, interpreter->op_count +
                PARROT_PROF_EXTRA,
                sizeof(ProfData), prof_sort_f);
        for (j = 0; j < interpreter->op_count + PARROT_PROF_EXTRA; j++) {
            UINTVAL n = interpreter->profile->data[j].numcalls;
            FLOATVAL t = interpreter->profile->data[j].time;
            if (n > 0) {
                op_count++;
                call_count += n;
                sum_time += t;

                k = interpreter->profile->data[j].op;
                PIO_printf(interpreter, "  %5d  %-20s  %7vu  %10vf  %10vf\n",
                        k,
                        k == (int)interpreter->op_count ?
                        "Exception" :
                        interpreter->op_info_table[k].full_name,
                        n,
                        t,
                        (FLOATVAL)(t / n)
                        );
            }
        }

        PIO_printf(interpreter, "  -----  -----------------     -------  ----------  ----------\n");
        PIO_printf(interpreter, "  %5vu  %-20s  %7vu  %10vf  %10vf\n",
                op_count,
                "",
                call_count,
                sum_time,
                (FLOATVAL)(sum_time / (FLOATVAL)call_count)
                );
    }
}

static void
print_debug(int status, void *p)
{
    Parrot_Interp interpreter = (Parrot_Interp) p;
    if (Interp_flags_TEST(interpreter, PARROT_DEBUG_FLAG)) {
        /* Give the souls brave enough to activate debugging an earful
         * about GC. */

        PIO_eprintf(interpreter, "*** Parrot VM: Dumping GC info ***\n");
        PDB_info(interpreter);
    }
}

void
Parrot_runcode(struct Parrot_Interp *interpreter, int argc, char *argv[])
{
    /* Debugging mode nonsense. */
    if (Interp_flags_TEST(interpreter, PARROT_DEBUG_FLAG)) {
        if (Interp_flags_TEST(interpreter, PARROT_BOUNDS_FLAG)) {
            PIO_eprintf(interpreter,
                    "*** Parrot VM: Bounds checking enabled. ***\n");
        }
        if (Interp_flags_TEST(interpreter, PARROT_TRACE_FLAG)) {
            PIO_eprintf(interpreter, "*** Parrot VM: Tracing enabled. ***\n");
        }
        PIO_eprintf(interpreter, "*** Parrot VM: ");
        switch (interpreter->run_core) {
            case PARROT_SLOW_CORE:
                PIO_eprintf(interpreter, "Slow core");
                break;
            case PARROT_FAST_CORE:
                PIO_eprintf(interpreter, "Fast core");
                break;
            case PARROT_PREDEREF_CORE:
                PIO_eprintf(interpreter, "Predereferencing core");
                break;
            case PARROT_SWITCH_CORE:
                PIO_eprintf(interpreter, "Switch core");
                break;
            case PARROT_CGP_CORE:
                PIO_eprintf(interpreter, "CGP core");
                break;
            case PARROT_CGOTO_CORE:
                PIO_eprintf(interpreter, "CGoto core");
                break;
            case PARROT_JIT_CORE:
                PIO_eprintf(interpreter, "JIT core");
                break;
            case PARROT_EXEC_CORE:
                PIO_eprintf(interpreter, "EXEC core");
                break;
        }
        PIO_eprintf(interpreter, " ***\n");
    }

    /* Set up @ARGS (or whatever this language calls it).
       XXX Should this be Array or PerlArray?             */

    setup_argv(interpreter, argc, argv);

#if EXEC_CAPABLE

    /* s. runops_exec interpreter.c */
    if (Interp_core_TEST(interpreter, PARROT_EXEC_CORE)) {
        extern int Parrot_exec_run;
        Parrot_exec_run = 1;
    }

#endif

    /*
     * If any profile information was gathered, print it out
     * before exiting, then print debug infos if turned on
     */
    Parrot_on_exit(print_debug,   interpreter);
    Parrot_on_exit(print_profile, interpreter);

    /* Let's kick the tires and light the fires--call interpreter.c:runops. */
    runops(interpreter,  0);
}


/* XXX Doesn't handle arguments with spaces */
static char*
argv_join(char ** argv)
{
    char* command;
    char* p;
    int space = 0;
    int i;

    for (i = 0; argv[i]; i++)
        space += strlen(argv[i]) + 1;

    command = (char*) malloc(space == 0 ? 1 : space);
    p = command;
    for (i = 0; argv[i]; i++) {
        strcpy(p, argv[i]);
        p += strlen(argv[i]);
        *(p++) = ' ';
    }
    if (p > command) p--;
    *p = '\0';
    return command;
}

void
Parrot_debug(struct Parrot_Interp *interpreter, int argc, char ** argv)
{
    PDB_t *pdb;
    const char *command;

    pdb = (PDB_t *)mem_sys_allocate_zeroed(sizeof(PDB_t));

    interpreter->pdb = pdb;
    pdb->cur_opcode = interpreter->code->byte_code;

    /* Parrot_setup_argv(interpreter, argc, argv); */
    PDB_init(interpreter, argv_join(argv));
    PDB_disassemble(interpreter,NULL);
    while (!(pdb->state & PDB_EXIT)) {
        PDB_get_command(interpreter);
        command = pdb->cur_command;
        PDB_run_command(interpreter, command);
    }
}

void
Parrot_disassemble(struct Parrot_Interp *interpreter)
{
    PDB_t *pdb;
    PDB_line_t *line;
    char *c;

    pdb = (PDB_t *)mem_sys_allocate_zeroed(sizeof(PDB_t));

    interpreter->pdb = pdb;
    pdb->cur_opcode = interpreter->code->byte_code;

    PDB_disassemble(interpreter, NULL);
    line = pdb->file->line;

    while (line->next) {
        /* If it has a label print it */
        if (line->label)
            PIO_printf(interpreter, "L%li:\t", line->label->number);
        c = pdb->file->source + line->source_offset;
        while (*c != '\n' && c)
            PIO_printf(interpreter, "%c", *(c++));
        PIO_printf(interpreter, "\n");
        line = line->next;
    }
    return;
}

/*=for api embed Parrot_run_native(Parrot_Interp, native_func_t func)

Run the C function B<func> through the program [enternative, end].
This ensures that the function is run with the same setup as in other
run loops.

*/
void
Parrot_run_native(Parrot_Interp interpreter, native_func_t func)
{
    static opcode_t program_code[2];
    struct PackFile *          pf;

    program_code[0] = interpreter->op_lib->op_code("enternative", 0);
    program_code[1] = 0; /* end */
    pf = PackFile_new(0);
    pf->cur_cs = (struct PackFile_ByteCode *)
	(pf->PackFuncs[PF_BYTEC_SEG].new_seg)(pf, "code", 1);
    pf->byte_code = pf->cur_cs->base.data = program_code;
    pf->cur_cs->base.size = 2;
    Parrot_loadbc(interpreter, pf);
    run_native = func;
    runops(interpreter, 0);
}
/*
 * Local variables:
 * c-indentation-style: bsd
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 * vim: expandtab shiftwidth=4:
 */
