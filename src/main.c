/**
 * @file main.c
 * @brief Khronos client
 * @version 0.5
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pocketsphinx.h>
#include <portaudio.h>
#include <sphinxbase/err.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <tritium.h>
#include <unistd.h>

#include "audio.h"
#include "command.h"
#include "color.h"
#include "debug.h"
#include "util.h"

cst_voice *register_cmu_us_rms(const char *str);

static void say(const char *str)
{
    cst_voice* v = register_cmu_us_rms(NULL);
    tritium_textToSpeech(str, v, "play");
}

static const char* recognizeFromFile(ps_decoder_t *ps, const char* fileName)
{
    int16 adbuf[2048];
    FILE *file = NULL;
    const char *hyp = NULL;
    size_t k = 0;

    if ((file = fopen(fileName, "rb")) == NULL)
        fprintf(stderr, "Failed to open file '%s' for reading\n", fileName);

    // verify .wav file?  I trust libsndfile to make a valid one
    ps_start_utt(ps);
    bool uttStarted = false;

    while ((k = fread(adbuf, sizeof(int16), 2048, file)) > 0)
    {
        ps_process_raw(ps, adbuf, k, false, false);
        bool inSpeech = ps_get_in_speech(ps);
        if (inSpeech && !uttStarted) uttStarted = true;
        if (!inSpeech && uttStarted)
        {
            ps_end_utt(ps);
            hyp = ps_get_hyp(ps, NULL);
            ps_start_utt(ps);
            uttStarted = false;
        }
    }
    ps_end_utt(ps);

    if (uttStarted)
        hyp = ps_get_hyp(ps, NULL);

    fclose(file);
    return hyp;
}


int runKhronos(PaStream *stream, AudioData *data, AudioSnippet *sampleBlock, ps_decoder_t *ps)
{
    int err = 0;
    bool sampleComplete = false;
    FileInfo info = createFileInfo(getTmpDir());

    if ((err = processStream(stream, data, sampleBlock, info.fd, &sampleComplete)))
    {
        fprintf(stderr, "Error recording FLAC file: %d\n", err);
        return err;
    }
    else if (sampleComplete)
    {
        const char *text = recognizeFromFile(ps, info.filename);
        if (text)
            fprintf(stdout, "Recognized text: %s\n", text);
        else
            puts(RED_TEXT("No text recognized."));
        if (text)
        {
            bool said = false;
            for (size_t i = 0; i < ARRAY_SIZE(commands); ++i)
            {
                const Command *p = commands + i;
                if (strstr(text, p->cmd))
                {
                    const char* out = p->fn();
                    fprintf(stdout, "%s\n", out);
                    say(out);
                    said = true;
                }
            }
            // there was some text, but a response was unknown
            if (!said)
                say("I could not understand what you said.");
        }
        sampleComplete = false;
    }
    close(info.fd);
    unlink(info.filename);
    free((void*) info.filename);
    return err;
}

static int parseArgs(int argc, char **argv)
{
    while (argc--)
    {
        if (streq("--help", argv[argc]) || streq("-h", argv[argc]) || streq("help", argv[argc]))
        {
            puts(printHelp());
            exit(EXIT_SUCCESS);
        }
        if (streq("--version", argv[argc]) || streq("-v", argv[argc]) || streq("version", argv[argc]))
        {
            puts(printVersionInfo());
            exit(EXIT_SUCCESS);
        }
        if (streq("--say", argv[argc]) || streq("-s", argv[argc]) || streq("say", argv[argc]))
        {
            say(argv[argc + 1]);
            exit(EXIT_SUCCESS);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    debug_print("%s\n", "Program started.");
    int err = 0;
    srand ((unsigned) time(NULL));

    debug_print("%s\n", "Handling command line arguments.");
    if (argc > 1)
    {
        if ((err = parseArgs(argc, argv)))
        {
            fprintf(stderr, "%s\n", "Error parsing command line arguments.");
            return err;
        }
    }

    debug_print("%s\n", "Initializing pocketsphinx configuration.");
    cmd_ln_t *config = cmd_ln_init(NULL, ps_args(), true,
                                   "-hmm", MODELDIR "/en-us/en-us",
                                   "-lm", MODELDIR "/en-us/en-us.lm.bin",
                                   "-dict", MODELDIR "/en-us/cmudict-en-us.dict",
                                   "-logfn", "/dev/null",
                                   NULL);
    if (!config)
    {
        fprintf(stderr, "%s\n", "Failed to create config, see log for details.");
        return -1;
    }

    debug_print("%s\n", "Initializing pocketsphinx decoder.");
    ps_decoder_t *ps = ps_init(config);
    if (!ps)
    {
        fprintf(stderr, "%s\n", "Failed to create recognizer, update CMU Sphinx.");
        return -1;
    }

    debug_print("%s\n", "Initializing PortAudio2 data.");
    AudioData *data = allocAudioData();
    AudioSnippet *sampleBlock = &((AudioSnippet)
                                  {
                                      .size = 0,
                                      .snippet = NULL
                                  });

    debug_print("%s\n", "Initializing PortAudio2 device stream.");
    PaStream *stream = NULL;
    err = init(&stream, data, sampleBlock);
    if (err)
    {
        fprintf(stderr, "%s\n", "Failed to initialize device stream");
        return err;
    }


    // main program loop
    debug_print("%s\n", "Initialization complete.");
    while (!err)
        err = runKhronos(stream, data, sampleBlock, ps);

    debug_print("%s\n", "Staring cleanup process.");
    //cleanup
    freeAudioData(&data);
    free(sampleBlock->snippet);
    free(stream);
    ps_free(ps);
    cmd_ln_free_r(config);
    Pa_Terminate();
    puts(Pa_GetErrorText(err));

    debug_print("%s\n", "Cleanup process complete, exiting program.");
    return err;
}
