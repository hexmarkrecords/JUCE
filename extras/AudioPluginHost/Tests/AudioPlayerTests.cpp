#include <JuceHeader.h>
using namespace juce;
#define PIP_DEMO_UTILITIES_INCLUDED 1
static std::unique_ptr<InputStream> createAssetInputStream (const char* name)
{
    return File (AUDIO_PLAYER_ASSETS).getChildFile (name).createInputStream();
}
#include "../Source/Plugins/AudioPlayer.h"
#include <iostream>

static void require (bool condition, const char* description)
{
    if (! condition)
        throw std::runtime_error (description);
}

int main()
{
    ScopedJuceInitialiser_GUI initialise;
    const auto file = File::getCurrentWorkingDirectory().getNonexistentChildFile ("AudioPlayerTest", ".wav");
    try
    {
        AudioBuffer<float> source (2, 15003); // Longer than the Sampler's 10-second cap.
        for (int i = 0; i < source.getNumSamples(); ++i)
        {
            source.setSample (0, i, 0.125f + static_cast<float> (i % 31) / 128.0f);
            source.setSample (1, i, -0.125f - static_cast<float> (i % 17) / 128.0f);
        }
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> writer (wav.createWriterFor (file.createOutputStream().release(),
                                                                      1000.0, 2, 24, {}, 0));
        require (writer != nullptr && writer->writeFromAudioSampleBuffer (source, 0, source.getNumSamples()), "write fixture");
        writer.reset();
        AudioFormatManager manager;
        manager.registerBasicFormats();
        AudioPlayerProcessor player;
        player.prepareToPlay (1000.0, 257);
        require (player.setFile (file, manager), "load WAV");
        AudioBuffer<float> block (2, 257);
        MidiBuffer midi;
        player.processBlock (block, midi);
        require (block.getMagnitude (0, 257) == 0.0f, "initially stopped");

        int cursor = 0;
        auto check = [&] (int begin, int end)
        {
            for (int i = begin; i < end; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                    require (std::abs (block.getSample (ch, i) - source.getSample (ch, cursor)) < 1.0e-6f,
                             "full-duration, original-speed, single-source sample comparison");
                cursor = (cursor + 1) % source.getNumSamples();
            }
        };
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 1), 13);
        midi.addEvent (MidiMessage::noteOff (1, 60), 20);
        midi.addEvent (MidiMessage::noteOn (2, 67, (uint8) 127), 40);
        midi.addEvent (MidiMessage::pitchWheel (1, 16383), 50);
        player.processBlock (block, midi);
        require (block.getMagnitude (0, 13) == 0.0f, "sample accurate start");
        check (13, 257);
        for (int b = 0; b < 28016; ++b) // More than two hours of programme time, with > 470 loops.
        {
            if (b % 97 == 0)
                midi.addEvent (MidiMessage::noteOn (1, 72, (uint8) 100), 11);
            if (b % 101 == 0)
                midi.addEvent (MidiMessage::noteOff (1, 60), 0);
            player.processBlock (block, midi);
            check (0, 257);
        }
        midi.addEvent (MidiMessage::noteOn (3, 60, (uint8) 127), 101);
        player.processBlock (block, midi);
        check (0, 101);
        require (block.getMagnitude (101, 156) == 0.0f, "same note across channels stops exactly at event");
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        player.processBlock (block, midi);
        cursor = 0;
        check (0, 257);
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 10);
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 20);
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 30);
        player.processBlock (block, midi);
        check (0, 10);
        require (block.getMagnitude (10, 10) == 0.0f, "repeated toggles stop");
        cursor = 0;
        check (20, 30);
        require (block.getMagnitude (30, 227) == 0.0f, "repeated toggles never overlap");

        player.prepareToPlay (2000.0, 257);
        AudioBuffer<double> doubles (2, 257);
        midi.addEvent (MidiMessage::noteOn (1, 24, (uint8) 127), 0);
        player.processBlock (doubles, midi);
        for (int i = 0; i < 257; ++i)
        {
            const auto index = i / 2;
            const auto expected = (source.getSample (0, index) + source.getSample (0, index + (i % 2))) * 0.5f;
            require (std::abs (doubles.getSample (0, i) - expected) < 1.0e-6, "sample-rate conversion and double precision");
        }
        require (! player.setFile (file.getSiblingFile ("missing.wav"), manager),
                 "bad file rejected");
        require (player.setBusesLayout (AudioProcessor::BusesLayout { {}, { AudioChannelSet::mono() } }), "mono layout");
        player.prepareToPlay (500.0, 257);
        block.setSize (1, 257);
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        player.processBlock (block, midi);
        for (int i = 0; i < 257; ++i)
            require (std::abs (block.getSample (0, i) - (source.getSample (0, i * 2) + source.getSample (1, i * 2)) * 0.5f) < 1.0e-6f,
                     "downsampling and stereo to mono");

        // Reopening the editor must not reload the file or stop an active source.
        {
            std::unique_ptr<AudioProcessorEditor> editor (player.createEditor());
            player.processBlock (block, midi);
            require (block.getMagnitude (0, 257) > 0.0f, "editor reopen preserves playback");
        }
        for (int i = 0; i < 4; ++i)
            require (player.setFile (file, manager), "rapid replacement");
        player.processBlock (block, midi);
        require (block.getMagnitude (0, 257) == 0.0f, "replacement resets stopped");
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        player.processBlock (block, midi);
        for (int i = 0; i < 257; ++i)
            require (std::abs (block.getSample (0, i) - (source.getSample (0, i * 2) + source.getSample (1, i * 2)) * 0.5f) < 1.0e-6f,
                     "replacement starts at beginning");
        // Graph state uses these exact processor calls (wrapped by InternalPlugin).
        MemoryBlock saved;
        player.getStateInformation (saved);
        auto savedXml = AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
        require (savedXml != nullptr && savedXml->getStringAttribute ("filePath") == file.getFullPathName(),
                 "selected file path saved while playing");
        AudioPlayerProcessor restored;
        restored.prepareToPlay (1000.0, 257);
        restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        AudioBuffer<float> recalled (2, 257);
        restored.processBlock (recalled, midi);
        require (recalled.getMagnitude (0, 257) == 0.0f, "recalled file starts stopped");
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        restored.processBlock (recalled, midi);
        for (int i = 0; i < 257; ++i)
            for (int ch = 0; ch < 2; ++ch)
                require (std::abs (recalled.getSample (ch, i) - source.getSample (ch, i)) < 1.0e-6f,
                         "recalled file plays from start without opening editor");
        {
            std::unique_ptr<AudioProcessorEditor> editor (restored.createEditor());
            restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
            MessageManager::getInstance()->runDispatchLoopUntil (10);
            restored.processBlock (recalled, midi);
            require (recalled.getMagnitude (0, 257) == 0.0f, "restore with open editor resets stopped");
        }
        XmlElement missing ("AudioPlayerState");
        missing.setAttribute ("version", 1);
        const auto missingPath = file.getSiblingFile ("missing.wav").getFullPathName();
        missing.setAttribute ("filePath", missingPath);
        MemoryBlock missingState;
        AudioProcessor::copyXmlToBinary (missing, missingState);
        restored.setStateInformation (missingState.getData(), static_cast<int> (missingState.getSize()));
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        restored.processBlock (recalled, midi);
        require (recalled.getMagnitude (0, 257) == 0.0f, "missing restored file stays silent");
        restored.getStateInformation (missingState);
        auto missingXml = AudioProcessor::getXmlFromBinary (missingState.getData(), static_cast<int> (missingState.getSize()));
        require (missingXml != nullptr && missingXml->getStringAttribute ("filePath") == missingPath,
                 "missing file reference survives resave");
        restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        MemoryBlock validState;
        restored.getStateInformation (validState);
        const char malformed[] = "invalid plugin state";
        restored.setStateInformation (malformed, sizeof (malformed));
        MemoryBlock afterInvalid;
        restored.getStateInformation (afterInvalid);
        require (validState == afterInvalid, "malformed state ignored");
        restored.setStateInformation (nullptr, 0);
        restored.getStateInformation (afterInvalid);
        auto emptyXml = AudioProcessor::getXmlFromBinary (afterInvalid.getData(), static_cast<int> (afterInvalid.getSize()));
        require (emptyXml != nullptr && emptyXml->getStringAttribute ("filePath").isEmpty(), "legacy empty state clears file");

        require (player.setSample (nullptr, manager), "unload");
        player.processBlock (block, midi);
        require (block.getMagnitude (0, 257) == 0.0f, "unload stops");
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        player.processBlock (block, midi);
        require (block.getMagnitude (0, 257) == 0.0f, "note without file is silent");

        // Exercise the untouched original processor and its default sample/note-off behaviour.
        SamplerAudioProcessor sampler;
        sampler.prepareToPlay (44100.0, 257);
        AudioBuffer<float> original (2, 257);
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 127), 0);
        original.clear();
        sampler.processBlock (original, midi);
        require (original.getMagnitude (0, 257) > 0.0f, "original Sampler starts");
        midi.clear();
        midi.addEvent (MidiMessage::noteOff (1, 60), 0);
        original.clear();
        sampler.processBlock (original, midi);
        midi.clear();
        for (int i = 0; i < 300; ++i)
        {
            original.clear();
            sampler.processBlock (original, midi);
        }
        require (original.getMagnitude (0, 257) == 0.0f, "original Sampler still tails off and stops");
        std::cout << "PASS: loading, start/stop/restart, full duration, 470+ loops / two-hour accelerated soak, monophony, note-off, rate conversion, mono/stereo, double precision, invalid file, file-state recall / missing / legacy / malformed state, original Sampler.\n";
    }
    catch (const std::exception& error)
    {
        file.deleteFile();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    file.deleteFile();
    return 0;
}
