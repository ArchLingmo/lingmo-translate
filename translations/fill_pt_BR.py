#!/usr/bin/env python3
"""Fills lingmo-translate_pt_BR.ts from the table below (run after lupdate)."""
import sys
import xml.etree.ElementTree as ET

PT = {
    "%1 (fixed)": "%1 (fixo)",
    "Detected: %1": "Detectado: %1",
    "Detecting the language…": "Detectando o idioma…",
    "Spoken language": "Idioma falado",
    "Instant translation from Japanese (anime, dramas): Opus-MT Japanese → English":
        "Tradução instantânea do japonês (animes, doramas): Opus-MT japonês → inglês",
    "Instant translation from Chinese: Opus-MT Chinese → English": "Tradução instantânea do chinês: Opus-MT chinês → inglês",
    "Instant translation from Korean: Opus-MT Korean → English": "Tradução instantânea do coreano: Opus-MT coreano → inglês",
    "Downloading the Japanese → English translator (290 MB)…": "Baixando o tradutor japonês → inglês (290 MB)…",
    "Downloading the Chinese → English translator (290 MB)…": "Baixando o tradutor chinês → inglês (290 MB)…",
    "Downloading the Korean → English translator (290 MB)…": "Baixando o tradutor coreano → inglês (290 MB)…",
    "For a faster dubbing from %1, download its instant translator in Models (80 MB).":
        "Para uma dublagem mais rápida do %1, baixe o tradutor instantâneo dele em Modelos (80 MB).",
    "%1 / 30 s": "%1 / 30 s",
    "%1 · licence %2": "%1 · licença %2",
    "%1 s": "%1 s",
    "%1 s, noise margin %2 dB.": "%1 s, margem sobre o ruído de %2 dB.",
    "20 to 30 seconds of you speaking alone, without music. Cloning someone else's voice is not allowed.":
        "De 20 a 30 segundos só com você falando, sem música. Clonar a voz de outra pessoa não é permitido.",
    "An instant draft, then the local AI model refines it (about 1 s more). Your cloned voice always uses the fast one: both do not fit in the video memory.":
        "Um rascunho na hora, depois o modelo de IA local refina (cerca de 1 s a mais). A sua voz clonada usa sempre a rápida: as duas não cabem juntas na memória de vídeo.",
    "A translated voice in your headphones over the original, which plays lower. About 1 s behind on short lines, 2–3 s on long ones.":
        "Uma voz traduzida nos seus fones por cima do original, que fica mais baixo. Cerca de 1 s de atraso nas falas curtas, 2–3 s nas longas.",
    "Audio (*.wav *.mp3 *.ogg *.opus *.m4a *.flac)": "Áudio (*.wav *.mp3 *.ogg *.opus *.m4a *.flac)",
    "Auto": "Auto",
    "Best quality": "Melhor qualidade",
    "Both at once: subtitles of what they say, and your words translated through the translator microphone.":
        "Os dois ao mesmo tempo: legendas do que dizem e as suas palavras traduzidas pelo microfone tradutor.",
    "Call": "Chamada",
    "Call %1": "Chamada %1",
    "Cancel": "Cancelar",
    "Cancelled. It continues where it stopped the next time.": "Cancelado. Na próxima vez continua de onde parou.",
    "Checking the translation engine…": "Verificando o motor de tradução…",
    "Checking the video memory…": "Verificando a memória de vídeo…",
    "Choose…": "Escolher…",
    "Cloned voice": "Voz clonada",
    "Could not write %1": "Não foi possível gravar %1",
    "CPU, slow": "CPU, lento",
    "Creating the translation engine…": "Criando o motor de tradução…",
    "Creating the voice engine environment…": "Criando o ambiente do motor de voz…",
    "Delete": "Apagar",
    "Delete “%1”?": "Apagar “%1”?",
    "Detect automatically": "Detectar automaticamente",
    "Done moving": "Concluir",
    "Download": "Baixar",
    "Downloaded once, used offline": "Baixados uma vez, usados sem internet",
    "Downloading speech recognition (Whisper small, 181 MB)…": "Baixando o reconhecimento de fala (Whisper small, 181 MB)…",
    "Downloading the English → Portuguese translator (290 MB)…": "Baixando o tradutor inglês → português (290 MB)…",
    "Downloading the Portuguese → English translator (290 MB)…": "Baixando o tradutor português → inglês (290 MB)…",
    "Downloading the voice model (about 3 GB)…": "Baixando o modelo de voz (cerca de 3 GB)…",
    "Downloading voice detection (Silero VAD)…": "Baixando a detecção de voz (Silero VAD)…",
    "Download the models once": "Baixe os modelos uma vez",
    "Download the translation models first (about 520 MB, once).": "Baixe primeiro os modelos de tradução (cerca de 520 MB, uma vez só).",
    "Download what is needed": "Baixar o necessário",
    "Drag to move": "Arraste para mover",
    "Dubbing %1": "Dublando %1",
    "End of speech detected": "Fim da fala detectado",
    "English": "Inglês",
    "Voices for dubbing (piper: Faber in Portuguese, Joe in English)": "Vozes para a dublagem (piper: Faber em português, Joe em inglês)",
    "Downloading a voice for dubbing (63 MB)…": "Baixando uma voz para a dublagem (63 MB)…",
    "Download the translation models first (about 650 MB, once).": "Baixe primeiro os modelos de tradução (cerca de 650 MB, uma vez só).",
    "eSpeak (robotic, any language)": "eSpeak (robótica, qualquer idioma)",
    "Everything runs on this computer: no audio or text is uploaded.": "Tudo roda neste computador: nenhum áudio ou texto é enviado.",
    "Fast": "Rápida",
    "Fastest": "Mais rápida",
    "Final translation": "Tradução final",
    "Free the GPU": "Liberar a GPU",
    "French": "Francês",
    "From": "De",
    "Good recording": "Boa gravação",
    "GPU": "GPU",
    "Import a recording of your voice": "Importar uma gravação da sua voz",
    "Import my recording…": "Importar minha gravação…",
    "In Meet, Teams or Zoom pick the microphone “Lingmo Tradutor”. It speaks your words, translated. It is removed when you stop.":
        "No Meet, Teams ou Zoom escolha o microfone “Lingmo Tradutor”. Ele fala as suas palavras, traduzidas. Ele some quando você parar.",
    "Installed": "Instalado",
    "Installing the translation engine (about 170 MB)…": "Instalando o motor de tradução (cerca de 170 MB)…",
    "Installing the voice engine (about 6 GB, this takes a while)…": "Instalando o motor de voz (cerca de 6 GB, demora um pouco)…",
    "Instant translation": "Tradução instantânea",
    "Instant translation: Opus-MT English ↔ Portuguese": "Tradução instantânea: Opus-MT inglês ↔ português",
    "In use": "Em uso",
    "Italian": "Italiano",
    "German": "Alemão",
    "Translate to": "Traduzir para",
    "Loading your voice…": "Carregando a sua voz…",
    "Ready: speak now": "Pronto: pode falar",
    "Your voice runs on the processor (no room on the GPU): it will be slow.": "A sua voz está rodando no processador (sem espaço na GPU): vai ficar lenta.",
    "Hear them": "Ouvir a outra pessoa",
    "Dubbed": "Dublado",
    "Subtitles only": "Só legendas",
    "Both ways: you hear them translated (dubbed or subtitles), and your words go translated through the translator microphone.":
        "Nos dois sentidos: você ouve a outra pessoa traduzida (dublada ou em legendas), e a sua fala vai traduzida pelo microfone tradutor.",
    "Japanese": "Japonês",
    "Chinese": "Chinês",
    "Korean": "Coreano",
    "Last": "Última",
    "Listen dubbed": "Ouvir dublado",
    "Listening": "Ouvindo",
    "Listening…": "Ouvindo…",
    "Listen to": "Ouvir",
    "Live subtitles": "Legendas ao vivo",
    "Live translation": "Tradução ao vivo",
    "Loading speech recognition…": "Carregando o reconhecimento de fala…",
    "Loading the quality translator…": "Carregando o tradutor de qualidade…",
    "Look for apps playing sound": "Procurar apps tocando som",
    "Median": "Mediana",
    "Meta+Shift+T": "Meta+Shift+T",
    "Microphone": "Microfone",
    "Milliseconds after the speaker stopped: the last sentence and the median.": "Milissegundos depois que a pessoa parou de falar: a última frase e a mediana.",
    "Mode": "Modo",
    "Models": "Modelos",
    "Move subtitles": "Mover legendas",
    "My voice: %1": "Minha voz: %1",
    "No file chosen": "Nenhum arquivo escolhido",
    "noise margin %1 dB": "margem sobre o ruído %1 dB",
    "Not enough video memory for the quality translator: using the fast one this time.": "Sem memória de vídeo para o tradutor de qualidade: usando o rápido desta vez.",
    "Nothing translated yet": "Nada traduzido ainda",
    "(not ready)": "(não está pronta)",
    "Not ready": "Não está pronto",
    "No voice profile yet": "Nenhum perfil de voz ainda",
    "One app": "Um app",
    "Only you can create your voice profile. It stays on this computer, readable only by you.":
        "Só você pode criar o seu perfil de voz. Ele fica neste computador, legível só por você.",
    "Open": "Abrir",
    "optional": "opcional",
    "Opus-MT only: about 0.1 s per sentence, sometimes too literal.": "Só o Opus-MT: cerca de 0,1 s por frase, às vezes literal demais.",
    "Original audio": "Áudio original",
    "Original text": "Texto original",
    "Pause": "Pausar",
    "Paused": "Pausado",
    "Portuguese (Brazil)": "Português (Brasil)",
    "Prepare": "Preparar",
    "Preparing…": "Preparando…",
    "Preparing the translator for fast use…": "Preparando o tradutor para uso rápido…",
    "Preparing your voice… (once; then every sentence reuses it)": "Preparando a sua voz… (uma vez só; depois cada frase reaproveita)",
    "Profile name": "Nome do perfil",
    "Profile name (e.g. My voice)": "Nome do perfil (ex.: Minha voz)",
    "Profiles are kept in ~/.local/share/lingmo/voices, readable only by you. Nothing is uploaded.":
        "Os perfis ficam em ~/.local/share/lingmo/voices, legíveis só por você. Nada é enviado.",
    "Python 3 was not found. Install it with “sudo pacman -S python”.": "O Python 3 não foi encontrado. Instale com “sudo pacman -S python”.",
    "Read a short text aloud for about 25 seconds. Only your own voice can be cloned, and it stays on this computer.":
        "Leia um texto curto em voz alta por cerca de 25 segundos. Só a sua própria voz pode ser clonada, e ela fica neste computador.",
    "Read this aloud at your normal pace, as in a meeting. Use the microphone you use for calls.":
        "Leia em voz alta no seu ritmo normal, como numa reunião. Use o microfone das suas chamadas.",
    "Ready": "Pronto",
    "Ready.": "Pronto.",
    "Record again": "Gravar de novo",
    "Recording works now; to speak with your voice, install the voice engine (about 6 GB, shared with Studio's dubbing).":
        "Já dá para gravar; para falar com a sua voz, instale o motor de voz (cerca de 6 GB, o mesmo da dublagem do Studio).",
    "Record my voice": "Gravar minha voz",
    "Record new profile": "Gravar novo perfil",
    "Record your voice": "Grave a sua voz",
    "Resume": "Continuar",
    "Save": "Salvar",
    "Spanish": "Espanhol",
    "Speaking %1": "Falando %1",
    "Speak translated": "Falar traduzido",
    "Speech: CPU": "Fala: CPU",
    "Speech: GPU": "Fala: GPU",
    "Speech recognition and translation run on this computer. They need about 520 MB, downloaded only now.":
        "O reconhecimento de fala e a tradução rodam neste computador. Precisam de cerca de 520 MB, baixados só agora.",
    "Speech recognition: Whisper small": "Reconhecimento de fala: Whisper small",
    "Stage": "Etapa",
    "Start": "Iniciar",
    "Start, and the translated lines appear here and on the screen.": "Inicie, e as frases traduzidas aparecem aqui e na tela.",
    "Starting…": "Iniciando…",
    "Start recording": "Começar a gravar",
    "Stop": "Parar",
    "Subtitles": "Legendas",
    "Subtitles and voice for videos, meetings and calls, on this computer": "Legendas e voz para vídeos, reuniões e chamadas, neste computador",
    "Text size": "Tamanho do texto",
    "The audio server refused to create the microphone (%1).": "O servidor de áudio recusou criar o microfone (%1).",
    "The download failed: %1": "O download falhou: %1",
    "The installation failed:": "A instalação falhou:",
    "The installer could not be started.": "Não foi possível iniciar o instalador.",
    "Their audio": "Áudio deles",
    "The recording and the prepared voice are deleted.": "A gravação e a voz preparada são apagadas.",
    "The recording could be better": "A gravação pode ficar melhor",
    "The recording is saved. Install the voice engine (Models page) to finish the profile.": "A gravação foi salva. Instale o motor de voz (página Modelos) para concluir o perfil.",
    "The translation engine is not installed yet.": "O motor de tradução ainda não está instalado.",
    "The translation engine stopped (code %1).": "O motor de tradução parou (código %1).",
    "The translator microphone is on": "O microfone tradutor está ligado",
    "The voice engine is not installed": "O motor de voz não está instalado",
    "The voice engine is not installed (Models page).": "O motor de voz não está instalado (página Modelos).",
    "The voice profile is not ready yet. Record it again or pick another voice.": "O perfil de voz ainda não está pronto. Grave de novo ou escolha outra voz.",
    "They speak": "Eles falam",
    "This is a recording of my own voice": "Esta gravação é da minha própria voz",
    "Timings": "Tempos",
    "To": "Para",
    "Too loud, the sound clips: move a little away from the microphone.": "Alto demais, o som estoura: afaste-se um pouco do microfone.",
    "Too many silences: read continuously.": "Silêncio demais: leia sem parar.",
    "Too much background noise: find a quieter place.": "Ruído de fundo demais: procure um lugar mais silencioso.",
    "Too quiet: come closer to the microphone or raise its volume.": "Baixo demais: chegue mais perto do microfone ou aumente o volume dele.",
    "Too short: read the whole text (at least 18 seconds).": "Curta demais: leia o texto todo (pelo menos 18 segundos).",
    "Transcribed": "Transcrito",
    "Translate": "Traduzir",
    "Translated subtitles on top of everything, about 1–2 s behind the speech.": "Legendas traduzidas por cima de tudo, cerca de 1–2 s depois da fala.",
    "Translating %1": "Traduzindo %1",
    "Translation": "Tradução",
    "Translation: AI": "Tradução: IA",
    "Translation engine and voice detection (Silero VAD)": "Motor de tradução e detecção de voz (Silero VAD)",
    "Translation: Opus-MT": "Tradução: Opus-MT",
    "Translation stopped": "Tradução encerrada",
    "Translator microphone on": "Microfone tradutor ligado",
    "Translator microphone on: it speaks for you": "Microfone tradutor ligado: ele fala por você",
    "Translator microphone paused": "Microfone tradutor em pausa",
    "Unloads the AI models held in the video memory (Ollama, Voice Studio)": "Descarrega os modelos de IA que ocupam a memória de vídeo (Ollama, Estúdio de Voz)",
    "Your voice needs %1 of video memory and only %2 MB are free. \"Free the GPU\" unloads the AI models (Ollama, Voice Studio), or use a piper voice.":
        "A sua voz precisa de %1 de memória de vídeo e só há %2 MB livres. \"Liberar a GPU\" descarrega os modelos de IA (Ollama, Estúdio de Voz), ou use uma voz piper.",
    "Use": "Usar",
    "Use headphones": "Use fones de ouvido",
    "Voice": "Voz",
    "Voice profile ready": "Perfil de voz pronto",
    "Voice profiles": "Perfis de voz",
    "Voice starts playing": "Voz começa a tocar",
    "Voice synthesized": "Voz sintetizada",
    "Waiting for %1 to play sound…": "Esperando %1 tocar som…",
    "Whisper (MIT) recognises speech; Opus-MT by the University of Helsinki (CC-BY 4.0) translates instantly; the optional AI refinement uses your Ollama model (qwen3.5, Apache-2.0); piper (MIT, CC0 voices) and Chatterbox (MIT) speak. Manage them also in Settings › AI.":
        "O Whisper (MIT) reconhece a fala; o Opus-MT da Universidade de Helsinque (CC-BY 4.0) traduz na hora; o refinamento opcional por IA usa o seu modelo do Ollama (qwen3.5, Apache-2.0); o piper (MIT, vozes CC0) e o Chatterbox (MIT) falam. Gerencie também em Configurações › IA.",
    "Whole system": "Sistema todo",
    "Without headphones the translated voice and the original mix in the room.": "Sem fones, a voz traduzida e o original se misturam no ambiente.",
    "Without headphones your microphone hears the other side and the translator translates them as if it were you.":
        "Sem fones, o seu microfone ouve o outro lado e o tradutor traduz a fala deles como se fosse você.",
    "Your own voice (Chatterbox, shared with Studio's dubbing)": "A sua própria voz (Chatterbox, o mesmo da dublagem do Studio)",
    "Your own voice, to speak translated in calls": "A sua própria voz, para falar traduzido em chamadas",
    "Your voice is ready (%1)": "A sua voz está pronta (%1)",
    "Your voice needs about 4 GB of video memory, and Ollama is holding %1 MB of it.": "A sua voz precisa de cerca de 4 GB de memória de vídeo, e o Ollama está ocupando %1 MB dela.",
    "Your voice needs about 4 GB of video memory and only %1 MB are free. Close the program using the GPU, or use a piper voice.":
        "A sua voz precisa de cerca de 4 GB de memória de vídeo e só há %1 MB livres. Feche o programa que está usando a GPU, ou use uma voz do piper.",
    "You speak": "Você fala",
    "You speak, they hear the translation from the “Lingmo Tradutor” microphone, in your own voice or a synthetic one. About 3–5 s.":
        "Você fala, e eles ouvem a tradução pelo microfone “Lingmo Tradutor”, com a sua própria voz ou uma sintética. Cerca de 3–5 s.",
    "Preparing your voice for OmniVoice (only the first time)…": "Preparando a sua voz para o OmniVoice (só na primeira vez)…",
    "The voice profile has no recording. Record it again or pick another voice.":
        "O perfil de voz não tem gravação. Grave de novo ou escolha outra voz.",
    "OmniVoice is chosen in Settings but is not turned on in Voice Studio: your voice uses Chatterbox.":
        "O OmniVoice está escolhido nas Configurações, mas não está ligado no Estúdio de Voz: a sua voz usa o Chatterbox.",
    "about 3 GB": "cerca de 3 GB",
    "about 4 GB": "cerca de 4 GB",
    "Your voice needs %1 of video memory and only %2 MB are free. Close the program using the GPU, or use a piper voice.":
        "A sua voz precisa de %1 de memória de vídeo e só há %2 MB livres. Feche o programa que está usando a GPU, ou use uma voz do piper.",
    "Your voice needs %1 of video memory, and Ollama is holding %2 MB of it.":
        "A sua voz precisa de %1 de memória de vídeo, e o Ollama está ocupando %2 MB dela.",
    "I speak": "Eu falo",
    "They hear": "Eles ouvem",
    "An instant draft, then the local AI model refines it (about 1 s more). Your cloned voice uses the fast one when both do not fit in the video memory.":
        "Um rascunho na hora, depois o modelo de IA local refina (cerca de 1 s a mais). A sua voz clonada usa a rápida quando as duas não cabem juntas na memória de vídeo.",
    "OmniVoice is not turned on in Voice Studio: using Chatterbox.": "O OmniVoice não está ligado no Estúdio de Voz: usando o Chatterbox.",
    "Engine: %1 (change it in Settings › AI › Cloned voice).": "Motor: %1 (troque em Configurações › IA › Voz clonada).",
    # the hardware recommendation (hwprofile.h, ~/.config/lingmoos/hardware.conf)
    "Whisper small: fastest": "Whisper small: o mais rápido",
    "Whisper base: for a slow processor": "Whisper base: para um processador lento",
    "Whisper large-v3-turbo, compressed: accurate": "Whisper large-v3-turbo, compactado: preciso",
    "Whisper large-v3-turbo: most accurate": "Whisper large-v3-turbo: o mais preciso",
    "Downloading the speech model %1 for this mode; a model already on this computer is used meanwhile.":
        "Baixando o modelo de fala %1 para este modo; enquanto isso, é usado um modelo que já está neste computador.",
    "Little free video memory beside your voice: speech recognition uses the compressed model this time.":
        "Pouca memória de vídeo livre ao lado da sua voz: o reconhecimento de fala usa o modelo compactado desta vez.",
    "Not enough video memory for the quality translator beside your voice: using the fast one this time.":
        "Não há memória de vídeo para o tradutor de qualidade ao lado da sua voz: usando o rápido desta vez.",
    "Download what is needed (%1)": "Baixar o necessário (%1)",
    "Recommended for this computer": "Recomendado para este computador",
    "Downloading speech recognition (Whisper %1, %2)…": "Baixando o reconhecimento de fala (Whisper %1, %2)…",
    "Speech recognition for this computer: Whisper %1": "Reconhecimento de fala para este computador: Whisper %1",
    "(recommended)": "(recomendado)",
    "no NVIDIA graphics card with CUDA": "sem placa de vídeo NVIDIA com CUDA",
    "%1, %2 GB": "%1, %2 GB",
    "instant draft refined by the AI model": "rascunho na hora refinado pelo modelo de IA",
    "instant translation only": "só a tradução instantânea",
    "your cloned voice": "a sua voz clonada",
    "a piper voice": "uma voz do piper",
    "Recommended for this computer (%1): %2.": "Recomendado para este computador (%1): %2.",
    "About %1 GB of video memory.": "Cerca de %1 GB de memória de vídeo.",
    "Everything runs on the processor.": "Tudo roda no processador.",
    "Speech recognition and translation run on this computer. They need about %1, downloaded only now.":
        "O reconhecimento de fala e a tradução rodam neste computador. Precisam de cerca de %1, baixados só agora.",
    "Use recommended": "Usar o recomendado",
    "Forget what you changed in this mode and use what suits this computer":
        "Esquecer o que você mudou neste modo e usar o que combina com este computador",
    "Speech recognition": "Reconhecimento de fala",
    "downloaded on first use": "baixado no primeiro uso",
    "Bigger models understand more and are slower on the processor.":
        "Modelos maiores entendem mais e são mais lentos no processador.",
    "Bigger models understand more and need more video memory: %1 GB for this one.":
        "Modelos maiores entendem mais e precisam de mais memória de vídeo: %1 GB para este.",
    "An instant draft, then the local AI model refines it (about 1 s more).":
        "Um rascunho na hora, depois o modelo de IA local refina (cerca de 1 s a mais).",
    "A cloned voice is not recommended on this computer: without room on a CUDA graphics card it runs on the processor, several seconds per sentence.":
        "Uma voz clonada não é recomendada neste computador: sem espaço numa placa de vídeo com CUDA, ela roda no processador, vários segundos por frase.",
    "Recommended: your own cloned voice. Record it once to use it.":
        "Recomendado: a sua própria voz clonada. Grave-a uma vez para usá-la.",
    "Dubbing uses piper voices, so the AI translator fits beside them.":
        "A dublagem usa vozes do piper, para o tradutor de IA caber ao lado delas.",
    # whisperhw.cpp (shared with Settings: the same wording as its pt_BR.ts)
    "Runs on the graphics card with %1 (%2 GB of video memory).": "Roda na placa de vídeo com %1 (%2 GB de memória de vídeo).",
    "Runs on the graphics card with %1.": "Roda na placa de vídeo com %1.",
    "%n processor core(s)": ["%n núcleo do processador", "%n núcleos do processador"],
    "Install %1 to use your %2 (up to ~%3× faster)": "Instale %1 para usar sua %2 (até ~%3× mais rápido)",
    "Install %1 to use your %2": "Instale %1 para usar sua %2",
    "Your %1 can't run whisper yet: ggml has no backend for it, so this model is for the processor (%2).":
        "Sua %1 ainda não roda o whisper: falta o backend do ggml para ela, então este modelo é para o processador (%2).",
    "The graphics card has too little video memory, so this model is for the processor (%1).":
        "A placa de vídeo tem pouca memória, então este modelo é para o processador (%1).",
    "The fastest model that still keeps up with the speech on the processor (%1).":
        "O melhor modelo que ainda acompanha a fala no processador (%1).",
    "~%1× real time": "~%1× o tempo real",
    "~%1 min per minute of audio": "~%1 min por minuto de áudio",
}


def main(path):
    tree = ET.parse(path)
    missing = []
    for m in tree.iter("message"):
        src = m.find("source").text or ""
        tr = m.find("translation")
        if src.startswith("Olá! Este é o meu perfil"):
            text = src  # the reading text is Portuguese already
        elif src in PT:
            text = PT[src]
        else:
            missing.append(src)
            continue
        if isinstance(text, list):  # numerus: one form per plural
            for child in list(tr):
                tr.remove(child)
            tr.text = None
            for form in text:
                ET.SubElement(tr, "numerusform").text = form
        else:
            tr.text = text
        tr.attrib.pop("type", None)
    tree.write(path, encoding="utf-8", xml_declaration=True)
    for s in missing:
        print("missing:", s)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
