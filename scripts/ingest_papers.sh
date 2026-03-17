#!/bin/bash
# Batch ingest agentic AI research papers into dsco knowledge base
# Usage: ./scripts/ingest_papers.sh [max_papers]

set -euo pipefail
DSCO="./dsco"
MAX=${1:-250}
COUNT=0
FAIL=0
SKIP=0

GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
DIM='\033[2m'
BOLD='\033[1m'
RESET='\033[0m'

ingest() {
    local title="$1"
    local url="$2"
    if [ $COUNT -ge $MAX ]; then return; fi
    COUNT=$((COUNT + 1))
    printf "${CYAN}[%03d]${RESET} %-70s " "$COUNT" "$title"

    local result
    result=$(curl -sL -o /tmp/dsco_kb_tmp.pdf -w "%{http_code}" "$url" 2>/dev/null) || result="000"

    if [ "$result" != "200" ]; then
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET} ${DIM}(HTTP $result)${RESET}\n"
        return
    fi

    local fsize=$(wc -c < /tmp/dsco_kb_tmp.pdf)
    if [ "$fsize" -lt 1000 ]; then
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET} ${DIM}(too small: ${fsize}B)${RESET}\n"
        return
    fi

    # Check dedup via hash
    local hash=$(shasum /tmp/dsco_kb_tmp.pdf 2>/dev/null | cut -c1-16)

    # Extract text with pdftotext
    local txt_path="/tmp/dsco_kb_${hash}.txt"
    pdftotext -layout /tmp/dsco_kb_tmp.pdf "$txt_path" 2>/dev/null

    if [ ! -s "$txt_path" ]; then
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET} ${DIM}(no text extracted)${RESET}\n"
        rm -f "$txt_path"
        return
    fi

    local words=$(wc -w < "$txt_path")
    local pages=$(grep -c $'\f' "$txt_path" 2>/dev/null || echo "1")
    pages=$((pages + 1))

    # Insert into SQLite directly (faster than going through dsco)
    local db="$HOME/.dsco/knowledge.db"
    local fname=$(basename "$url")

    # Check if already exists
    local existing=$(sqlite3 "$db" "SELECT id FROM documents WHERE title='$(echo "$title" | sed "s/'/''/g")' LIMIT 1;" 2>/dev/null)
    if [ -n "$existing" ]; then
        SKIP=$((SKIP + 1))
        printf "${YELLOW}SKIP${RESET} ${DIM}(already indexed, id=$existing)${RESET}\n"
        rm -f "$txt_path"
        return
    fi

    # Insert document
    sqlite3 "$db" "INSERT INTO documents(filename,hash,title,page_count) VALUES('$fname','$hash','$(echo "$title" | sed "s/'/''/g")',$pages);"
    local doc_id=$(sqlite3 "$db" "SELECT last_insert_rowid();")

    # Split on form feed and insert pages
    local page_num=0
    local page_content=""
    while IFS= read -r -d $'\f' page_content || [ -n "$page_content" ]; do
        page_num=$((page_num + 1))
        local wc=$(echo "$page_content" | wc -w)
        # Escape for SQLite
        local escaped=$(echo "$page_content" | sed "s/'/''/g")
        sqlite3 "$db" "INSERT OR REPLACE INTO pages(doc_id,page_num,content,word_count) VALUES($doc_id,$page_num,'$escaped',$wc);" 2>/dev/null || true
    done < "$txt_path"

    # Update page count
    sqlite3 "$db" "UPDATE documents SET page_count=$page_num WHERE id=$doc_id;"

    rm -f "$txt_path"
    printf "${GREEN}OK${RESET} ${DIM}(${pages}pp, ${words}w, id=$doc_id)${RESET}\n"
}

printf "\n${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}  AGENTIC AI RESEARCH PAPER INGESTION${RESET}\n"
printf "${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n\n"

# Ensure DB exists
mkdir -p "$HOME/.dsco"
$DSCO -m haiku 'Use kb_list' 2>/dev/null >/dev/null || true

# ── Category 1: LLM Agents & Tool Use ──────────────────────────────────
printf "\n${BOLD}── LLM Agents & Tool Use ──${RESET}\n"
ingest "ReAct: Synergizing Reasoning and Acting in LLMs" "https://arxiv.org/pdf/2210.03629"
ingest "Toolformer: LLMs Can Teach Themselves to Use Tools" "https://arxiv.org/pdf/2302.04761"
ingest "MRKL Systems: A Modular Neuro-Symbolic Architecture" "https://arxiv.org/pdf/2205.00445"
ingest "ART: Automatic Multi-step Reasoning and Tool-use" "https://arxiv.org/pdf/2303.09014"
ingest "Gorilla: Large Language Model Connected with APIs" "https://arxiv.org/pdf/2305.15334"
ingest "TaskMatrix.AI: Completing Tasks by Connecting Foundation Models" "https://arxiv.org/pdf/2303.16434"
ingest "HuggingGPT: Solving AI Tasks with ChatGPT and Friends" "https://arxiv.org/pdf/2303.17580"
ingest "ToolLLM: Facilitating LLMs to Master 16000+ APIs" "https://arxiv.org/pdf/2307.16789"
ingest "API-Bank: A Comprehensive Benchmark for Tool-Augmented LLMs" "https://arxiv.org/pdf/2304.08244"
ingest "Chameleon: Plug-and-Play Compositional Reasoning with LLMs" "https://arxiv.org/pdf/2304.09842"
ingest "RestGPT: Connecting LLMs with RESTful APIs" "https://arxiv.org/pdf/2306.06624"
ingest "ToolkenGPT: Augmenting Frozen LLMs with Tool Embeddings" "https://arxiv.org/pdf/2305.11554"
ingest "GPT4Tools: Teaching LLMs to Use Tools via Self-Instruction" "https://arxiv.org/pdf/2305.18752"
ingest "WebGPT: Browser-assisted Question-answering with Human Feedback" "https://arxiv.org/pdf/2112.09332"

# ── Category 2: Multi-Agent Systems ────────────────────────────────────
printf "\n${BOLD}── Multi-Agent Systems ──${RESET}\n"
ingest "CAMEL: Communicative Agents for Mind Exploration of Large Scale LLMs" "https://arxiv.org/pdf/2303.17760"
ingest "AutoGen: Enabling Next-Gen LLM Applications via Multi-Agent Conversation" "https://arxiv.org/pdf/2308.08155"
ingest "MetaGPT: Meta Programming for Multi-Agent Collaborative Framework" "https://arxiv.org/pdf/2308.00352"
ingest "ChatDev: Communicative Agents for Software Development" "https://arxiv.org/pdf/2307.07924"
ingest "AgentVerse: Facilitating Multi-Agent Collaboration" "https://arxiv.org/pdf/2308.10848"
ingest "Dynamic LLM-Agent Network for Multi-Agent Collaboration" "https://arxiv.org/pdf/2310.02170"
ingest "Generative Agents: Interactive Simulacra of Human Behavior" "https://arxiv.org/pdf/2304.03442"
ingest "War and Peace (WarAgent): LLM Multi-Agent Simulation of World Wars" "https://arxiv.org/pdf/2311.17227"
ingest "Multi-Agent Debate Improves Mathematical and Strategic Reasoning" "https://arxiv.org/pdf/2305.19118"
ingest "Corex: Pushing the Boundaries of Complex Reasoning through Multi-Model Collaboration" "https://arxiv.org/pdf/2310.00280"
ingest "LLM-Debate: Improving Factuality Through Multi-Agent Debate" "https://arxiv.org/pdf/2305.14325"
ingest "Encouraging Divergent Thinking in Large Language Models through Multi-Agent Debate" "https://arxiv.org/pdf/2305.19118"

# ── Category 3: Reasoning & Planning ──────────────────────────────────
printf "\n${BOLD}── Reasoning & Planning ──${RESET}\n"
ingest "Chain-of-Thought Prompting Elicits Reasoning in Large Language Models" "https://arxiv.org/pdf/2201.11903"
ingest "Tree of Thoughts: Deliberate Problem Solving with LLMs" "https://arxiv.org/pdf/2305.10601"
ingest "Graph of Thoughts: Solving Elaborate Problems with LLMs" "https://arxiv.org/pdf/2308.09687"
ingest "Self-Consistency Improves Chain of Thought Reasoning" "https://arxiv.org/pdf/2203.11171"
ingest "Plan-and-Solve Prompting" "https://arxiv.org/pdf/2305.04091"
ingest "Least-to-Most Prompting Enables Complex Reasoning in LLMs" "https://arxiv.org/pdf/2205.10625"
ingest "Faithful Chain-of-Thought Reasoning" "https://arxiv.org/pdf/2301.13379"
ingest "PAL: Program-aided Language Models" "https://arxiv.org/pdf/2211.10435"
ingest "Decomposed Prompting: A Modular Approach for Solving Complex Tasks" "https://arxiv.org/pdf/2210.02406"
ingest "Reasoning with Language Model is Planning with World Model" "https://arxiv.org/pdf/2305.14992"
ingest "Large Language Models as Analogical Reasoners" "https://arxiv.org/pdf/2310.01714"
ingest "Cumulative Reasoning with Language Models" "https://arxiv.org/pdf/2308.04371"
ingest "Algorithm of Thoughts: Enhancing Exploration of Ideas in LLMs" "https://arxiv.org/pdf/2308.10379"
ingest "Skeleton-of-Thought: LLMs Can Do Parallel Decoding" "https://arxiv.org/pdf/2307.15337"
ingest "Everything of Thoughts: Defying the Law of Penrose Triangle" "https://arxiv.org/pdf/2311.04254"

# ── Category 4: Code Generation & SWE Agents ──────────────────────────
printf "\n${BOLD}── Code Generation & SWE Agents ──${RESET}\n"
ingest "SWE-bench: Can Language Models Resolve Real-World GitHub Issues?" "https://arxiv.org/pdf/2310.06770"
ingest "CodeAct: Code Actions Elicit Better LLM Agents" "https://arxiv.org/pdf/2402.01030"
ingest "SWE-agent: Agent-Computer Interfaces Enable Automated Software Engineering" "https://arxiv.org/pdf/2405.15793"
ingest "OpenDevin: An Open Platform for AI Software Developers" "https://arxiv.org/pdf/2407.16741"
ingest "Codex: Evaluating Large Language Models Trained on Code" "https://arxiv.org/pdf/2107.03374"
ingest "StarCoder: May the Source Be with You!" "https://arxiv.org/pdf/2305.06161"
ingest "Code Llama: Open Foundation Models for Code" "https://arxiv.org/pdf/2308.12950"
ingest "WizardCoder: Empowering Code LLMs with Evol-Instruct" "https://arxiv.org/pdf/2306.08568"
ingest "Self-Debugging: Teaching LLMs to Debug Their Own Code" "https://arxiv.org/pdf/2304.05128"
ingest "InterCode: Standardizing and Benchmarking Interactive Coding" "https://arxiv.org/pdf/2306.14898"
ingest "Parsel: Algorithmic Reasoning with Language Models by Composing Decompositions" "https://arxiv.org/pdf/2212.10561"
ingest "RepoCoder: Repository-Level Code Completion Through Iterative Retrieval" "https://arxiv.org/pdf/2303.12570"
ingest "MapCoder: Multi-Agent Code Generation for Competitive Programming" "https://arxiv.org/pdf/2405.11403"

# ── Category 5: RAG & Retrieval ────────────────────────────────────────
printf "\n${BOLD}── RAG & Retrieval ──${RESET}\n"
ingest "Retrieval-Augmented Generation for Knowledge-Intensive NLP Tasks" "https://arxiv.org/pdf/2005.11401"
ingest "Self-RAG: Learning to Retrieve, Generate, and Critique" "https://arxiv.org/pdf/2310.11511"
ingest "RAPTOR: Recursive Abstractive Processing for Tree-Organized Retrieval" "https://arxiv.org/pdf/2401.18059"
ingest "ColBERT: Efficient and Effective Passage Search via Contextualized Late Interaction" "https://arxiv.org/pdf/2004.12832"
ingest "REPLUG: Retrieval-Augmented Black-Box Language Models" "https://arxiv.org/pdf/2301.12652"
ingest "Active Retrieval Augmented Generation" "https://arxiv.org/pdf/2305.06983"
ingest "Dense Passage Retrieval for Open-Domain Question Answering" "https://arxiv.org/pdf/2004.04906"
ingest "Lost in the Middle: How Language Models Use Long Contexts" "https://arxiv.org/pdf/2307.03172"
ingest "Corrective Retrieval Augmented Generation (CRAG)" "https://arxiv.org/pdf/2401.15884"
ingest "RAFT: Adapting Language Model to Domain Specific RAG" "https://arxiv.org/pdf/2403.10131"
ingest "Adaptive-RAG: Learning to Adapt Retrieval-Augmented LLMs" "https://arxiv.org/pdf/2403.14403"
ingest "HyDE: Precise Zero-Shot Dense Retrieval without Relevance Labels" "https://arxiv.org/pdf/2212.10496"
ingest "FreshLLMs: Refreshing Large Language Models with Search Engine Augmentation" "https://arxiv.org/pdf/2310.03214"

# ── Category 6: Agent Architectures ───────────────────────────────────
printf "\n${BOLD}── Agent Architectures ──${RESET}\n"
ingest "Voyager: An Open-Ended Embodied Agent with Large Language Models" "https://arxiv.org/pdf/2305.16291"
ingest "Reflexion: Language Agents with Verbal Reinforcement Learning" "https://arxiv.org/pdf/2303.11366"
ingest "Language Agent Tree Search (LATS)" "https://arxiv.org/pdf/2310.04406"
ingest "Cognitive Architectures for Language Agents" "https://arxiv.org/pdf/2309.02427"
ingest "The Rise and Potential of LLM Based Agents: A Survey" "https://arxiv.org/pdf/2309.07864"
ingest "A Survey on Large Language Model based Autonomous Agents" "https://arxiv.org/pdf/2308.11432"
ingest "Agent-FLAN: Designing Data and Methods of Effective Agent Tuning" "https://arxiv.org/pdf/2403.12881"
ingest "AgentTuning: Enabling Generalized Agent Abilities for LLMs" "https://arxiv.org/pdf/2310.12823"
ingest "FireAct: Toward Language Agent Fine-tuning" "https://arxiv.org/pdf/2310.05915"
ingest "DEPS: Describe, Explain, Plan and Select" "https://arxiv.org/pdf/2302.01560"
ingest "Inner Monologue: Embodied Reasoning through Planning with Language Models" "https://arxiv.org/pdf/2207.05608"
ingest "Ghost in the Minecraft: Generally Capable LLM Agents" "https://arxiv.org/pdf/2305.17144"
ingest "Describe, Explain, Plan and Select: Interactive Planning with LLMs" "https://arxiv.org/pdf/2302.01560"
ingest "ProAgent: Building Proactive Cooperative Agents with LLMs" "https://arxiv.org/pdf/2308.11339"
ingest "LLM-Planner: Few-Shot Grounded Planning for Embodied Agents" "https://arxiv.org/pdf/2212.04088"

# ── Category 7: Foundation Models ──────────────────────────────────────
printf "\n${BOLD}── Foundation Models ──${RESET}\n"
ingest "GPT-4 Technical Report" "https://arxiv.org/pdf/2303.08774"
ingest "LLaMA: Open and Efficient Foundation Language Models" "https://arxiv.org/pdf/2302.13971"
ingest "Llama 2: Open Foundation and Fine-Tuned Chat Models" "https://arxiv.org/pdf/2307.09288"
ingest "Mistral 7B" "https://arxiv.org/pdf/2310.06825"
ingest "Mixtral of Experts" "https://arxiv.org/pdf/2401.04088"
ingest "Gemini: A Family of Highly Capable Multimodal Models" "https://arxiv.org/pdf/2312.11805"
ingest "PaLM 2 Technical Report" "https://arxiv.org/pdf/2305.10403"
ingest "PaLM: Scaling Language Modeling with Pathways" "https://arxiv.org/pdf/2204.02311"
ingest "Phi-2: The Surprising Power of Small Language Models" "https://arxiv.org/pdf/2309.05463"
ingest "Qwen Technical Report" "https://arxiv.org/pdf/2309.16609"
ingest "DeepSeek-V2: A Strong, Economical, and Efficient Mixture-of-Experts" "https://arxiv.org/pdf/2405.04434"
ingest "Yi: Open Foundation Models by 01.AI" "https://arxiv.org/pdf/2403.04652"
ingest "OLMo: Accelerating the Science of Language Models" "https://arxiv.org/pdf/2402.00838"
ingest "Mamba: Linear-Time Sequence Modeling with Selective State Spaces" "https://arxiv.org/pdf/2312.00752"
ingest "RWKV: Reinventing RNNs for the Transformer Era" "https://arxiv.org/pdf/2305.13048"

# ── Category 8: Evaluation & Benchmarks ───────────────────────────────
printf "\n${BOLD}── Evaluation & Benchmarks ──${RESET}\n"
ingest "MMLU: Measuring Massive Multitask Language Understanding" "https://arxiv.org/pdf/2009.03300"
ingest "HumanEval: Evaluating Large Language Models Trained on Code" "https://arxiv.org/pdf/2107.03374"
ingest "AgentBench: Evaluating LLMs as Agents" "https://arxiv.org/pdf/2308.03688"
ingest "GAIA: A Benchmark for General AI Assistants" "https://arxiv.org/pdf/2311.12983"
ingest "MT-Bench and Chatbot Arena" "https://arxiv.org/pdf/2306.05685"
ingest "HELM: Holistic Evaluation of Language Models" "https://arxiv.org/pdf/2211.09110"
ingest "BIG-bench: Beyond the Imitation Game Benchmark" "https://arxiv.org/pdf/2206.04615"
ingest "TruthfulQA: Measuring How Models Mimic Human Falsehoods" "https://arxiv.org/pdf/2109.07958"
ingest "GSM8K: Training Verifiers to Solve Math Word Problems" "https://arxiv.org/pdf/2110.14168"
ingest "ARC: AI2 Reasoning Challenge" "https://arxiv.org/pdf/1803.05457"
ingest "HellaSwag: Can a Machine Really Finish Your Sentence?" "https://arxiv.org/pdf/1905.07830"
ingest "WinoGrande: An Adversarial Winograd Schema Challenge" "https://arxiv.org/pdf/1907.10641"
ingest "MATH: Measuring Mathematical Problem Solving" "https://arxiv.org/pdf/2103.03874"
ingest "WebArena: A Realistic Web Environment for Building Autonomous Agents" "https://arxiv.org/pdf/2307.13854"
ingest "OSWorld: Benchmarking Multimodal Agents for Open-Ended Tasks in Real Computer Environments" "https://arxiv.org/pdf/2404.07972"
ingest "τ-bench: A Benchmark for Tool-Agent-User Interaction in Real-World Domains" "https://arxiv.org/pdf/2406.12045"

# ── Category 9: Safety & Alignment ────────────────────────────────────
printf "\n${BOLD}── Safety & Alignment ──${RESET}\n"
ingest "Training Language Models to Follow Instructions with Human Feedback (InstructGPT)" "https://arxiv.org/pdf/2203.02155"
ingest "Constitutional AI: Harmlessness from AI Feedback" "https://arxiv.org/pdf/2212.08073"
ingest "Direct Preference Optimization (DPO)" "https://arxiv.org/pdf/2305.18290"
ingest "RLHF: Training a Helpful and Harmless Assistant" "https://arxiv.org/pdf/2204.05862"
ingest "Scaling Laws for Reward Model Overoptimization" "https://arxiv.org/pdf/2210.10760"
ingest "Red Teaming Language Models to Reduce Harms" "https://arxiv.org/pdf/2209.07858"
ingest "Sleeper Agents: Training Deceptive LLMs" "https://arxiv.org/pdf/2401.05566"
ingest "Weak-to-Strong Generalization" "https://arxiv.org/pdf/2312.09390"
ingest "The Alignment Problem from a Deep Learning Perspective" "https://arxiv.org/pdf/2209.00626"
ingest "RLAIF: Scaling Reinforcement Learning from Human Feedback with AI Feedback" "https://arxiv.org/pdf/2309.00267"
ingest "KTO: Model Alignment as Prospect Theoretic Optimization" "https://arxiv.org/pdf/2402.01306"
ingest "Representation Engineering: A Top-Down Approach to AI Transparency" "https://arxiv.org/pdf/2310.01405"

# ── Category 10: Prompt Engineering & Instruction Tuning ──────────────
printf "\n${BOLD}── Prompt Engineering & Instruction Tuning ──${RESET}\n"
ingest "FLAN: Finetuned Language Models Are Zero-Shot Learners" "https://arxiv.org/pdf/2109.01652"
ingest "Self-Instruct: Aligning Language Models with Self-Generated Instructions" "https://arxiv.org/pdf/2212.10560"
ingest "Alpaca: A Strong, Replicable Instruction-Following Model" "https://arxiv.org/pdf/2303.16199"
ingest "WizardLM: Empowering Large Language Models to Follow Complex Instructions" "https://arxiv.org/pdf/2304.12244"
ingest "LIMA: Less Is More for Alignment" "https://arxiv.org/pdf/2305.11206"
ingest "Orca: Progressive Learning from Complex Explanation Traces" "https://arxiv.org/pdf/2306.02707"
ingest "Platypus: Quick, Cheap, and Powerful Refinement of LLMs" "https://arxiv.org/pdf/2308.07317"
ingest "Large Language Models Are Human-Level Prompt Engineers" "https://arxiv.org/pdf/2211.01910"
ingest "Principled Instructions Are All You Need for Questioning LLMs" "https://arxiv.org/pdf/2312.16171"
ingest "DSPy: Compiling Declarative Language Model Calls into Self-Improving Pipelines" "https://arxiv.org/pdf/2310.03714"
ingest "OpenAI Prompt Engineering Guide - Structured Outputs" "https://arxiv.org/pdf/2408.11236"

# ── Category 11: Memory & State Management ────────────────────────────
printf "\n${BOLD}── Memory & State Management ──${RESET}\n"
ingest "MemGPT: Towards LLMs as Operating Systems" "https://arxiv.org/pdf/2310.08560"
ingest "Augmenting Language Models with Long-Term Memory" "https://arxiv.org/pdf/2306.07174"
ingest "Empowering LLM-based Machine Translation with Cultural Awareness" "https://arxiv.org/pdf/2305.14328"
ingest "RecurrentGPT: Interactive Generation of Arbitrarily Long Text" "https://arxiv.org/pdf/2305.13304"
ingest "Unleashing Infinite-Length Input Capacity for Large-Scale Language Models with Self-Controlled Memory" "https://arxiv.org/pdf/2304.13343"
ingest "Think-in-Memory: Recalling and Post-thinking Enable LLMs with Long-Term Memory" "https://arxiv.org/pdf/2311.08719"
ingest "Walking Down the Memory Maze: Beyond Context Limit through Interactive Reading" "https://arxiv.org/pdf/2310.05029"

# ── Category 12: Scaling Laws & Emergence ──────────────────────────────
printf "\n${BOLD}── Scaling Laws & Emergence ──${RESET}\n"
ingest "Scaling Laws for Neural Language Models" "https://arxiv.org/pdf/2001.08361"
ingest "Chinchilla: Training Compute-Optimal Large Language Models" "https://arxiv.org/pdf/2203.15556"
ingest "Emergent Abilities of Large Language Models" "https://arxiv.org/pdf/2206.07682"
ingest "Are Emergent Abilities of LLMs a Mirage?" "https://arxiv.org/pdf/2304.15004"
ingest "Scaling Data-Constrained Language Models" "https://arxiv.org/pdf/2305.16264"
ingest "Textbooks Are All You Need" "https://arxiv.org/pdf/2306.11644"
ingest "TinyStories: How Small Can Language Models Be and Still Speak Coherent English?" "https://arxiv.org/pdf/2305.07759"
ingest "Scaling Instruction-Finetuned Language Models (Flan-T5/PaLM)" "https://arxiv.org/pdf/2210.11416"

# ── Category 13: Knowledge Distillation ────────────────────────────────
printf "\n${BOLD}── Knowledge Distillation ──${RESET}\n"
ingest "Distilling the Knowledge in a Neural Network" "https://arxiv.org/pdf/1503.02531"
ingest "Lion: Adversarial Distillation of Proprietary LLMs" "https://arxiv.org/pdf/2305.12870"
ingest "Knowledge Distillation of Large Language Models" "https://arxiv.org/pdf/2306.08543"
ingest "LLM-Augmenter: Black-box LLM Augmentation" "https://arxiv.org/pdf/2302.12813"
ingest "Zephyr: Direct Distillation of LM Alignment" "https://arxiv.org/pdf/2310.16944"
ingest "Orca 2: Teaching Small Language Models How to Reason" "https://arxiv.org/pdf/2311.11045"

# ── Category 14: Multimodal ────────────────────────────────────────────
printf "\n${BOLD}── Multimodal Agents ──${RESET}\n"
ingest "LLaVA: Visual Instruction Tuning" "https://arxiv.org/pdf/2304.08485"
ingest "GPT-4V(ision) System Card" "https://arxiv.org/pdf/2309.17421"
ingest "CogVLM: Visual Expert for Pretrained Language Models" "https://arxiv.org/pdf/2311.03079"
ingest "InternVL: Scaling up Vision Foundation Models" "https://arxiv.org/pdf/2312.14238"
ingest "Video-LLaMA: An Instruction-tuned Audio-Visual Language Model" "https://arxiv.org/pdf/2306.02858"
ingest "MiniGPT-4: Enhancing Vision-Language Understanding" "https://arxiv.org/pdf/2304.10592"
ingest "BLIP-2: Bootstrapping Language-Image Pre-training" "https://arxiv.org/pdf/2301.12597"
ingest "Qwen-VL: A Versatile Vision-Language Model" "https://arxiv.org/pdf/2308.12966"
ingest "Set-of-Mark Prompting Unleashes Extraordinary Visual Grounding" "https://arxiv.org/pdf/2310.11441"
ingest "Ferret: Refer and Ground Anything Anywhere" "https://arxiv.org/pdf/2310.07704"

# ── Category 15: Quantization & Efficiency ─────────────────────────────
printf "\n${BOLD}── Quantization & Efficiency ──${RESET}\n"
ingest "GPTQ: Accurate Post-Training Quantization for Generative Pre-trained Transformers" "https://arxiv.org/pdf/2210.17323"
ingest "QLoRA: Efficient Finetuning of Quantized Language Models" "https://arxiv.org/pdf/2305.14314"
ingest "AWQ: Activation-aware Weight Quantization for LLM Compression" "https://arxiv.org/pdf/2306.00978"
ingest "FlashAttention: Fast and Memory-Efficient Exact Attention" "https://arxiv.org/pdf/2205.14135"
ingest "FlashAttention-2: Faster Attention with Better Parallelism" "https://arxiv.org/pdf/2307.08691"
ingest "LoRA: Low-Rank Adaptation of Large Language Models" "https://arxiv.org/pdf/2106.09685"
ingest "Speculative Decoding" "https://arxiv.org/pdf/2211.17192"
ingest "Medusa: Simple LLM Inference Acceleration Framework" "https://arxiv.org/pdf/2401.10774"
ingest "vLLM: Efficient Memory Management for Large Language Model Serving" "https://arxiv.org/pdf/2309.06180"
ingest "SGLang: Efficient Execution of Structured Language Model Programs" "https://arxiv.org/pdf/2312.07104"
ingest "PowerInfer: Fast Large Language Model Serving with a Consumer-grade GPU" "https://arxiv.org/pdf/2312.12456"

# ── Category 16: Structured Output & Function Calling ──────────────────
printf "\n${BOLD}── Structured Output & Function Calling ──${RESET}\n"
ingest "Efficient Guided Generation for Large Language Models" "https://arxiv.org/pdf/2307.09702"
ingest "Let Me Speak Freely? A Study on the Impact of Format Restrictions on LLM Performance" "https://arxiv.org/pdf/2408.02442"
ingest "NexusRaven: Function Calling LLM" "https://arxiv.org/pdf/2309.15337"
ingest "Semantic Kernel: Integrating LLMs with Conventional Programming" "https://arxiv.org/pdf/2312.04803"

# ── Category 17: World Models & Simulation ─────────────────────────────
printf "\n${BOLD}── World Models & Simulation ──${RESET}\n"
ingest "World Models for Autonomous Driving" "https://arxiv.org/pdf/2306.09223"
ingest "Language Models as World Models" "https://arxiv.org/pdf/2306.12672"
ingest "Language Models Meet World Models: Embodied Experiences Enhance Language Models" "https://arxiv.org/pdf/2305.10626"
ingest "Genie: Generative Interactive Environments" "https://arxiv.org/pdf/2402.15391"

# ── Category 18: Long Context & Attention ──────────────────────────────
printf "\n${BOLD}── Long Context & Attention ──${RESET}\n"
ingest "Longformer: The Long-Document Transformer" "https://arxiv.org/pdf/2004.05150"
ingest "Extending Context Window of Large Language Models via Positional Interpolation" "https://arxiv.org/pdf/2306.15595"
ingest "YaRN: Efficient Context Window Extension of Large Language Models" "https://arxiv.org/pdf/2309.00071"
ingest "LongRoPE: Extending LLM Context Window Beyond 2 Million Tokens" "https://arxiv.org/pdf/2402.13753"
ingest "Ring Attention with Blockwise Transformers for Near-Infinite Context" "https://arxiv.org/pdf/2310.01889"
ingest "Landmark Attention: Random-Access Infinite Context Length for Transformers" "https://arxiv.org/pdf/2305.16300"

# ── Category 19: Embeddings & Representation ──────────────────────────
printf "\n${BOLD}── Embeddings & Representation ──${RESET}\n"
ingest "Sentence-BERT: Sentence Embeddings using Siamese BERT-Networks" "https://arxiv.org/pdf/1908.10084"
ingest "E5: Text Embeddings by Weakly-Supervised Contrastive Pre-training" "https://arxiv.org/pdf/2212.03533"
ingest "Jina Embeddings 2: 8192-Token General-Purpose Text Embeddings" "https://arxiv.org/pdf/2310.19923"
ingest "Matryoshka Representation Learning" "https://arxiv.org/pdf/2205.13147"
ingest "Nomic Embed: Training a Reproducible Long Context Text Embedder" "https://arxiv.org/pdf/2402.01613"
ingest "Gecko: Versatile Text Embeddings Distilled from LLMs" "https://arxiv.org/pdf/2403.20327"

# ── Category 20: Synthetic Data & Self-Improvement ─────────────────────
printf "\n${BOLD}── Synthetic Data & Self-Improvement ──${RESET}\n"
ingest "Self-Play Fine-Tuning Converts Weak LLMs to Strong LLMs" "https://arxiv.org/pdf/2401.01335"
ingest "Magicoder: Source Code Is All You Need" "https://arxiv.org/pdf/2312.02120"
ingest "WaveCoder: Widespread And Versatile Enhanced Instruction Tuning with Refined Data Generation" "https://arxiv.org/pdf/2312.14187"
ingest "Aya Dataset: An Open-Access Collection for Multilingual Instruction Tuning" "https://arxiv.org/pdf/2402.06619"
ingest "Nemotron-4 340B Technical Report" "https://arxiv.org/pdf/2402.16819"
ingest "SOLAR 10.7B: Scaling Large Language Models with Simple yet Effective Depth Up-Scaling" "https://arxiv.org/pdf/2312.15166"
ingest "Phi-3 Technical Report" "https://arxiv.org/pdf/2404.14219"

# Done
printf "\n${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}  COMPLETE: ${GREEN}%d ingested${RESET} / ${RED}%d failed${RESET} / ${YELLOW}%d skipped${RESET} / %d total\n" \
    "$((COUNT - FAIL - SKIP))" "$FAIL" "$SKIP" "$COUNT"
printf "${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n\n"
