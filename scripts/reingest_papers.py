#!/usr/bin/env python3
"""Re-ingest papers that have 0 pages of content in the KB.
Uses pdftotext + direct sqlite3 bindings for proper text handling."""

import sqlite3
import subprocess
import os
import sys
import hashlib
import tempfile
import urllib.request

DB_PATH = os.path.expanduser("~/.dsco/knowledge.db")

# All papers with their arXiv PDF URLs
PAPERS = {
    "ReAct: Synergizing Reasoning and Acting in LLMs": "https://arxiv.org/pdf/2210.03629",
    "Toolformer: LLMs Can Teach Themselves to Use Tools": "https://arxiv.org/pdf/2302.04761",
    "MRKL Systems: A Modular Neuro-Symbolic Architecture": "https://arxiv.org/pdf/2205.00445",
    "ART: Automatic Multi-step Reasoning and Tool-use": "https://arxiv.org/pdf/2303.09014",
    "Gorilla: Large Language Model Connected with APIs": "https://arxiv.org/pdf/2305.15334",
    "TaskMatrix.AI: Completing Tasks by Connecting Foundation Models": "https://arxiv.org/pdf/2303.16434",
    "HuggingGPT: Solving AI Tasks with ChatGPT and Friends": "https://arxiv.org/pdf/2303.17580",
    "ToolLLM: Facilitating LLMs to Master 16000+ APIs": "https://arxiv.org/pdf/2307.16789",
    "API-Bank: A Comprehensive Benchmark for Tool-Augmented LLMs": "https://arxiv.org/pdf/2304.08244",
    "Chameleon: Plug-and-Play Compositional Reasoning with LLMs": "https://arxiv.org/pdf/2304.09842",
    "RestGPT: Connecting LLMs with RESTful APIs": "https://arxiv.org/pdf/2306.06624",
    "ToolkenGPT: Augmenting Frozen LLMs with Tool Embeddings": "https://arxiv.org/pdf/2305.11554",
    "GPT4Tools: Teaching LLMs to Use Tools via Self-Instruction": "https://arxiv.org/pdf/2305.18752",
    "WebGPT: Browser-assisted Question-answering with Human Feedback": "https://arxiv.org/pdf/2112.09332",
    "CAMEL: Communicative Agents for Mind Exploration of LLMs": "https://arxiv.org/pdf/2303.17760",
    "AutoGen: Enabling Next-Gen LLM Applications via Multi-Agent Conversation": "https://arxiv.org/pdf/2308.08155",
    "MetaGPT: Meta Programming for Multi-Agent Collaborative Framework": "https://arxiv.org/pdf/2308.00352",
    "ChatDev: Communicative Agents for Software Development": "https://arxiv.org/pdf/2307.07924",
    "AgentVerse: Facilitating Multi-Agent Collaboration": "https://arxiv.org/pdf/2308.10848",
    "Dynamic LLM-Agent Network for Multi-Agent Collaboration": "https://arxiv.org/pdf/2310.02170",
    "Generative Agents: Interactive Simulacra of Human Behavior": "https://arxiv.org/pdf/2304.03442",
    "WarAgent: LLM Multi-Agent Simulation of World Wars": "https://arxiv.org/pdf/2311.17227",
    "Multi-Agent Debate Improves Mathematical and Strategic Reasoning": "https://arxiv.org/pdf/2305.19118",
    "LLM-Debate: Improving Factuality Through Multi-Agent Debate": "https://arxiv.org/pdf/2305.14325",
    "Chain-of-Thought Prompting Elicits Reasoning in LLMs": "https://arxiv.org/pdf/2201.11903",
    "Tree of Thoughts: Deliberate Problem Solving with LLMs": "https://arxiv.org/pdf/2305.10601",
    "Graph of Thoughts": "https://arxiv.org/pdf/2308.09687",
    "Self-Consistency Improves Chain of Thought Reasoning": "https://arxiv.org/pdf/2203.11171",
    "Plan-and-Solve Prompting": "https://arxiv.org/pdf/2305.04091",
    "Least-to-Most Prompting Enables Complex Reasoning": "https://arxiv.org/pdf/2205.10625",
    "PAL: Program-aided Language Models": "https://arxiv.org/pdf/2211.10435",
    "Decomposed Prompting: Modular Approach for Complex Tasks": "https://arxiv.org/pdf/2210.02406",
    "Reasoning with LMs is Planning with World Model": "https://arxiv.org/pdf/2305.14992",
    "Skeleton-of-Thought: LLMs Can Do Parallel Decoding": "https://arxiv.org/pdf/2307.15337",
    "SWE-bench: Can LMs Resolve Real-World GitHub Issues?": "https://arxiv.org/pdf/2310.06770",
    "CodeAct: Code Actions Elicit Better LLM Agents": "https://arxiv.org/pdf/2402.01030",
    "SWE-agent: Agent-Computer Interfaces Enable Automated SWE": "https://arxiv.org/pdf/2405.15793",
    "Codex: Evaluating Large Language Models Trained on Code": "https://arxiv.org/pdf/2107.03374",
    "Code Llama: Open Foundation Models for Code": "https://arxiv.org/pdf/2308.12950",
    "Self-Debugging: Teaching LLMs to Debug Their Own Code": "https://arxiv.org/pdf/2304.05128",
    "RAG for Knowledge-Intensive NLP Tasks": "https://arxiv.org/pdf/2005.11401",
    "Self-RAG: Learning to Retrieve, Generate, and Critique": "https://arxiv.org/pdf/2310.11511",
    "RAPTOR: Recursive Abstractive Processing for Tree-Organized Retrieval": "https://arxiv.org/pdf/2401.18059",
    "ColBERT: Efficient and Effective Passage Search": "https://arxiv.org/pdf/2004.12832",
    "Dense Passage Retrieval for Open-Domain QA": "https://arxiv.org/pdf/2004.04906",
    "Lost in the Middle: How LMs Use Long Contexts": "https://arxiv.org/pdf/2307.03172",
    "CRAG: Corrective Retrieval Augmented Generation": "https://arxiv.org/pdf/2401.15884",
    "HyDE: Precise Zero-Shot Dense Retrieval": "https://arxiv.org/pdf/2212.10496",
    "Voyager: An Open-Ended Embodied Agent with LLMs": "https://arxiv.org/pdf/2305.16291",
    "Reflexion: Language Agents with Verbal Reinforcement Learning": "https://arxiv.org/pdf/2303.11366",
    "LATS: Language Agent Tree Search": "https://arxiv.org/pdf/2310.04406",
    "Cognitive Architectures for Language Agents": "https://arxiv.org/pdf/2309.02427",
    "The Rise and Potential of LLM Based Agents: A Survey": "https://arxiv.org/pdf/2309.07864",
    "Survey on LLM-based Autonomous Agents": "https://arxiv.org/pdf/2308.11432",
    "AgentTuning: Enabling Generalized Agent Abilities for LLMs": "https://arxiv.org/pdf/2310.12823",
    "FireAct: Toward Language Agent Fine-tuning": "https://arxiv.org/pdf/2310.05915",
    "Inner Monologue: Embodied Reasoning through Planning": "https://arxiv.org/pdf/2207.05608",
    "ProAgent: Building Proactive Cooperative Agents with LLMs": "https://arxiv.org/pdf/2308.11339",
    "GPT-4 Technical Report": "https://arxiv.org/pdf/2303.08774",
    "LLaMA: Open and Efficient Foundation Language Models": "https://arxiv.org/pdf/2302.13971",
    "Llama 2: Open Foundation and Fine-Tuned Chat Models": "https://arxiv.org/pdf/2307.09288",
    "Mistral 7B": "https://arxiv.org/pdf/2310.06825",
    "Mixtral of Experts": "https://arxiv.org/pdf/2401.04088",
    "Gemini: A Family of Highly Capable Multimodal Models": "https://arxiv.org/pdf/2312.11805",
    "PaLM 2 Technical Report": "https://arxiv.org/pdf/2305.10403",
    "DeepSeek-V2: Strong Economical Efficient MoE LM": "https://arxiv.org/pdf/2405.04434",
    "Mamba: Linear-Time Sequence Modeling": "https://arxiv.org/pdf/2312.00752",
    "RWKV: Reinventing RNNs for the Transformer Era": "https://arxiv.org/pdf/2305.13048",
    "OLMo: Accelerating the Science of Language Models": "https://arxiv.org/pdf/2402.00838",
    "MMLU: Measuring Massive Multitask Language Understanding": "https://arxiv.org/pdf/2009.03300",
    "AgentBench: Evaluating LLMs as Agents": "https://arxiv.org/pdf/2308.03688",
    "GAIA: A Benchmark for General AI Assistants": "https://arxiv.org/pdf/2311.12983",
    "MT-Bench and Chatbot Arena": "https://arxiv.org/pdf/2306.05685",
    "HELM: Holistic Evaluation of Language Models": "https://arxiv.org/pdf/2211.09110",
    "TruthfulQA: Measuring How Models Mimic Human Falsehoods": "https://arxiv.org/pdf/2109.07958",
    "WebArena: A Realistic Web Environment for Building Agents": "https://arxiv.org/pdf/2307.13854",
    "OSWorld: Benchmarking Multimodal Agents": "https://arxiv.org/pdf/2404.07972",
    "InstructGPT: Training LMs to Follow Instructions with RLHF": "https://arxiv.org/pdf/2203.02155",
    "Constitutional AI: Harmlessness from AI Feedback": "https://arxiv.org/pdf/2212.08073",
    "DPO: Direct Preference Optimization": "https://arxiv.org/pdf/2305.18290",
    "Sleeper Agents: Training Deceptive LLMs": "https://arxiv.org/pdf/2401.05566",
    "Weak-to-Strong Generalization": "https://arxiv.org/pdf/2312.09390",
    "Representation Engineering: Top-Down AI Transparency": "https://arxiv.org/pdf/2310.01405",
    "Self-Instruct: Aligning LMs with Self-Generated Instructions": "https://arxiv.org/pdf/2212.10560",
    "LIMA: Less Is More for Alignment": "https://arxiv.org/pdf/2305.11206",
    "DSPy: Compiling Declarative Language Model Calls": "https://arxiv.org/pdf/2310.03714",
    "LLMs Are Human-Level Prompt Engineers": "https://arxiv.org/pdf/2211.01910",
    "MemGPT: Towards LLMs as Operating Systems": "https://arxiv.org/pdf/2310.08560",
    "Augmenting Language Models with Long-Term Memory": "https://arxiv.org/pdf/2306.07174",
    "Scaling Laws for Neural Language Models": "https://arxiv.org/pdf/2001.08361",
    "Chinchilla: Training Compute-Optimal Large Language Models": "https://arxiv.org/pdf/2203.15556",
    "Emergent Abilities of Large Language Models": "https://arxiv.org/pdf/2206.07682",
    "Are Emergent Abilities of LLMs a Mirage?": "https://arxiv.org/pdf/2304.15004",
    "Textbooks Are All You Need": "https://arxiv.org/pdf/2306.11644",
    "FlashAttention: Fast and Memory-Efficient Exact Attention": "https://arxiv.org/pdf/2205.14135",
    "FlashAttention-2: Faster Attention with Better Parallelism": "https://arxiv.org/pdf/2307.08691",
    "LoRA: Low-Rank Adaptation of Large Language Models": "https://arxiv.org/pdf/2106.09685",
    "QLoRA: Efficient Finetuning of Quantized Language Models": "https://arxiv.org/pdf/2305.14314",
    "vLLM: Efficient Memory Management for LLM Serving": "https://arxiv.org/pdf/2309.06180",
    "SGLang: Efficient Execution of Structured LM Programs": "https://arxiv.org/pdf/2312.07104",
    "LLaVA: Visual Instruction Tuning": "https://arxiv.org/pdf/2304.08485",
    "BLIP-2: Bootstrapping Language-Image Pre-training": "https://arxiv.org/pdf/2301.12597",
    "Set-of-Mark Prompting Unleashes Visual Grounding": "https://arxiv.org/pdf/2310.11441",
    "Sentence-BERT: Sentence Embeddings using Siamese BERT": "https://arxiv.org/pdf/1908.10084",
    "E5: Text Embeddings by Contrastive Pre-training": "https://arxiv.org/pdf/2212.03533",
    "Matryoshka Representation Learning": "https://arxiv.org/pdf/2205.13147",
    "Zephyr: Direct Distillation of LM Alignment": "https://arxiv.org/pdf/2310.16944",
    "Orca 2: Teaching Small Language Models How to Reason": "https://arxiv.org/pdf/2311.11045",
    "YaRN: Efficient Context Window Extension": "https://arxiv.org/pdf/2309.00071",
    "LongRoPE: Extending Context Beyond 2M Tokens": "https://arxiv.org/pdf/2402.13753",
    "Ring Attention: Near-Infinite Context": "https://arxiv.org/pdf/2310.01889",
    "Self-Play Fine-Tuning: Weak to Strong LLMs": "https://arxiv.org/pdf/2401.01335",
    "Phi-3 Technical Report": "https://arxiv.org/pdf/2404.14219",
    # Additional important papers
    "Attention Is All You Need": "https://arxiv.org/pdf/1706.03762",
    "BERT: Pre-training of Deep Bidirectional Transformers": "https://arxiv.org/pdf/1810.04805",
    "GPT-3: Language Models are Few-Shot Learners": "https://arxiv.org/pdf/2005.14165",
    "Anthropic RLHF: Training a Helpful and Harmless Assistant": "https://arxiv.org/pdf/2204.05862",
    "Red Teaming Language Models to Reduce Harms": "https://arxiv.org/pdf/2209.07858",
    "RLAIF: Scaling RLHF with AI Feedback": "https://arxiv.org/pdf/2309.00267",
    "Distilling the Knowledge in a Neural Network": "https://arxiv.org/pdf/1503.02531",
    "BIG-bench: Beyond the Imitation Game Benchmark": "https://arxiv.org/pdf/2206.04615",
    "GSM8K: Training Verifiers to Solve Math Word Problems": "https://arxiv.org/pdf/2110.14168",
    "MATH: Measuring Mathematical Problem Solving": "https://arxiv.org/pdf/2103.03874",
    "Longformer: The Long-Document Transformer": "https://arxiv.org/pdf/2004.05150",
    "GPTQ: Post-Training Quantization for GPT": "https://arxiv.org/pdf/2210.17323",
    "AWQ: Activation-aware Weight Quantization": "https://arxiv.org/pdf/2306.00978",
    "Speculative Decoding": "https://arxiv.org/pdf/2211.17192",
    "FLAN: Finetuned Language Models Are Zero-Shot Learners": "https://arxiv.org/pdf/2109.01652",
    "WizardLM: Empowering LLMs to Follow Complex Instructions": "https://arxiv.org/pdf/2304.12244",
    "Orca: Progressive Learning from Complex Explanation Traces": "https://arxiv.org/pdf/2306.02707",
    "CogVLM: Visual Expert for Pretrained Language Models": "https://arxiv.org/pdf/2311.03079",
    "Qwen-VL: A Versatile Vision-Language Model": "https://arxiv.org/pdf/2308.12966",
    "PaLM: Scaling Language Modeling with Pathways": "https://arxiv.org/pdf/2204.02311",
    "Qwen Technical Report": "https://arxiv.org/pdf/2309.16609",
    "StarCoder: May the Source Be with You": "https://arxiv.org/pdf/2305.06161",
    "WizardCoder: Empowering Code LLMs with Evol-Instruct": "https://arxiv.org/pdf/2306.08568",
    "InterCode: Standardizing Interactive Coding Benchmarks": "https://arxiv.org/pdf/2306.14898",
    "FreshLLMs: Refreshing LLMs with Search Engine Augmentation": "https://arxiv.org/pdf/2310.03214",
    "RAFT: Adapting Language Model to Domain Specific RAG": "https://arxiv.org/pdf/2403.10131",
    "Adaptive-RAG: Learning to Adapt Retrieval-Augmented LLMs": "https://arxiv.org/pdf/2403.14403",
    "Agent-FLAN: Designing Data for Effective Agent Tuning": "https://arxiv.org/pdf/2403.12881",
    "Ghost in the Minecraft: Generally Capable LLM Agents": "https://arxiv.org/pdf/2305.17144",
    "LLM-Planner: Few-Shot Grounded Planning for Embodied Agents": "https://arxiv.org/pdf/2212.04088",
    "Yi: Open Foundation Models by 01.AI": "https://arxiv.org/pdf/2403.04652",
    "Scaling Instruction-Finetuned Language Models": "https://arxiv.org/pdf/2210.11416",
    "KTO: Model Alignment as Prospect Theoretic Optimization": "https://arxiv.org/pdf/2402.01306",
    "Nomic Embed: Reproducible Long Context Text Embedder": "https://arxiv.org/pdf/2402.01613",
    "Gecko: Versatile Text Embeddings Distilled from LLMs": "https://arxiv.org/pdf/2403.20327",
    "Magicoder: Source Code Is All You Need": "https://arxiv.org/pdf/2312.02120",
    "Medusa: Simple LLM Inference Acceleration": "https://arxiv.org/pdf/2401.10774",
    "PowerInfer: Fast LLM Serving with Consumer GPU": "https://arxiv.org/pdf/2312.12456",
    "NexusRaven: Function Calling LLM": "https://arxiv.org/pdf/2309.15337",
    "Genie: Generative Interactive Environments": "https://arxiv.org/pdf/2402.15391",
    "MiniGPT-4: Enhancing Vision-Language Understanding": "https://arxiv.org/pdf/2304.10592",
    "Ferret: Refer and Ground Anything Anywhere": "https://arxiv.org/pdf/2310.07704",
    "Video-LLaMA: Instruction-tuned Audio-Visual LM": "https://arxiv.org/pdf/2306.02858",
    "InternVL: Scaling up Vision Foundation Models": "https://arxiv.org/pdf/2312.14238",
    "RecurrentGPT: Interactive Generation of Long Text": "https://arxiv.org/pdf/2305.13304",
    "Scaling Data-Constrained Language Models": "https://arxiv.org/pdf/2305.16264",
    "TinyStories: How Small Can LMs Be": "https://arxiv.org/pdf/2305.07759",
    "SOLAR 10.7B: Depth Up-Scaling": "https://arxiv.org/pdf/2312.15166",
    "Lion: Adversarial Distillation of Proprietary LLMs": "https://arxiv.org/pdf/2305.12870",
    "Aya Dataset: Open-Access Multilingual Instruction Tuning": "https://arxiv.org/pdf/2402.06619",
    "Principled Instructions Are All You Need": "https://arxiv.org/pdf/2312.16171",
    "Everything of Thoughts: Defying Penrose Triangle": "https://arxiv.org/pdf/2311.04254",
    "Cumulative Reasoning with Language Models": "https://arxiv.org/pdf/2308.04371",
    "Algorithm of Thoughts: Enhancing Exploration in LLMs": "https://arxiv.org/pdf/2308.10379",
    "Large Language Models as Analogical Reasoners": "https://arxiv.org/pdf/2310.01714",
    "Faithful Chain-of-Thought Reasoning": "https://arxiv.org/pdf/2301.13379",
    "MapCoder: Multi-Agent Code Generation": "https://arxiv.org/pdf/2405.11403",
    "RepoCoder: Repository-Level Code Completion": "https://arxiv.org/pdf/2303.12570",
    "Active Retrieval Augmented Generation": "https://arxiv.org/pdf/2305.06983",
    "REPLUG: Retrieval-Augmented Black-Box LMs": "https://arxiv.org/pdf/2301.12652",
    "Alignment Problem from Deep Learning Perspective": "https://arxiv.org/pdf/2209.00626",
    "Scaling Laws for Reward Model Overoptimization": "https://arxiv.org/pdf/2210.10760",
    "Platypus: Quick Cheap Powerful Refinement of LLMs": "https://arxiv.org/pdf/2308.07317",
    "Parsel: Algorithmic Reasoning by Composing Decompositions": "https://arxiv.org/pdf/2212.10561",
    "World Models for Autonomous Driving": "https://arxiv.org/pdf/2306.09223",
    "Language Models Meet World Models": "https://arxiv.org/pdf/2305.10626",
    "Corex: Complex Reasoning through Multi-Model Collaboration": "https://arxiv.org/pdf/2310.00280",
    "Think-in-Memory: Recalling and Post-thinking": "https://arxiv.org/pdf/2311.08719",
    "Walking Down the Memory Maze": "https://arxiv.org/pdf/2310.05029",
    "Phi-2: Surprising Power of Small Language Models": "https://arxiv.org/pdf/2309.05463",
    "Knowledge Distillation of Large Language Models": "https://arxiv.org/pdf/2306.08543",
    "LLM-Augmenter: Black-box LLM Augmentation": "https://arxiv.org/pdf/2302.12813",
    "OpenDevin: Open Platform for AI Software Developers": "https://arxiv.org/pdf/2407.16741",
    "τ-bench: Tool-Agent-User Interaction Benchmark": "https://arxiv.org/pdf/2406.12045",
    "Jina Embeddings 2: 8192-Token Text Embeddings": "https://arxiv.org/pdf/2310.19923",
    "Let Me Speak Freely: Format Restrictions on LLM Performance": "https://arxiv.org/pdf/2408.02442",
    "GPT-4V System Card": "https://arxiv.org/pdf/2309.17421",
    "Landmark Attention: Random-Access Infinite Context": "https://arxiv.org/pdf/2305.16300",
    "Extending Context via Positional Interpolation": "https://arxiv.org/pdf/2306.15595",
    "Nemotron-4 340B Technical Report": "https://arxiv.org/pdf/2402.16819",
    "WaveCoder: Versatile Enhanced Instruction Tuning": "https://arxiv.org/pdf/2312.14187",
}

def download_pdf(url):
    """Download PDF to temp file, return path."""
    tmp = tempfile.NamedTemporaryFile(suffix='.pdf', delete=False)
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'dsco-kb/1.0'})
        with urllib.request.urlopen(req, timeout=30) as resp:
            tmp.write(resp.read())
        tmp.close()
        return tmp.name
    except Exception as e:
        tmp.close()
        os.unlink(tmp.name)
        return None

def extract_text(pdf_path):
    """Extract text using pdftotext."""
    try:
        result = subprocess.run(
            ['pdftotext', '-layout', pdf_path, '-'],
            capture_output=True, text=True, timeout=60
        )
        return result.stdout if result.returncode == 0 else None
    except:
        return None

def main():
    db = sqlite3.connect(DB_PATH)
    db.execute("PRAGMA journal_mode=WAL")

    # Ensure schema
    db.executescript("""
        CREATE TABLE IF NOT EXISTS documents(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT NOT NULL,
            hash TEXT UNIQUE,
            title TEXT,
            page_count INTEGER DEFAULT 0,
            created_at TEXT DEFAULT (datetime('now')),
            metadata TEXT DEFAULT '{}'
        );
        CREATE TABLE IF NOT EXISTS pages(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            doc_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            page_num INTEGER NOT NULL,
            content TEXT NOT NULL,
            content_type TEXT DEFAULT 'text',
            word_count INTEGER DEFAULT 0,
            UNIQUE(doc_id, page_num)
        );
    """)

    total = len(PAPERS)
    ok = 0
    skip = 0
    fail = 0

    for i, (title, url) in enumerate(PAPERS.items(), 1):
        # Check if already has content
        row = db.execute(
            "SELECT d.id, (SELECT COUNT(*) FROM pages WHERE doc_id=d.id AND word_count>0) as pg "
            "FROM documents d WHERE d.title=? LIMIT 1", (title,)
        ).fetchone()

        if row and row[1] > 0:
            skip += 1
            print(f"  [{i:03d}/{total}] SKIP {title[:60]:60s} (has {row[1]}pp)")
            continue

        # Download
        pdf_path = download_pdf(url)
        if not pdf_path:
            fail += 1
            print(f"  [{i:03d}/{total}] FAIL {title[:60]:60s} (download)")
            continue

        # Extract text
        text = extract_text(pdf_path)
        os.unlink(pdf_path)

        if not text or len(text) < 100:
            fail += 1
            print(f"  [{i:03d}/{total}] FAIL {title[:60]:60s} (no text)")
            continue

        # Hash for dedup
        h = hashlib.md5(text.encode()).hexdigest()[:16]

        # Delete old empty entry if exists
        if row:
            db.execute("DELETE FROM pages WHERE doc_id=?", (row[0],))
            db.execute("DELETE FROM documents WHERE id=?", (row[0],))

        # Check hash dedup
        existing = db.execute("SELECT id FROM documents WHERE hash=?", (h,)).fetchone()
        if existing:
            skip += 1
            print(f"  [{i:03d}/{total}] SKIP {title[:60]:60s} (hash dup)")
            continue

        # Insert document
        db.execute(
            "INSERT INTO documents(filename, hash, title, page_count) VALUES(?,?,?,0)",
            (os.path.basename(url), h, title)
        )
        doc_id = db.execute("SELECT last_insert_rowid()").fetchone()[0]

        # Split on form feed and insert pages
        raw_pages = text.split('\f')
        page_count = 0
        total_words = 0

        for pn, page_text in enumerate(raw_pages, 1):
            page_text = page_text.strip()
            if not page_text:
                continue
            page_count += 1
            wc = len(page_text.split())
            total_words += wc
            db.execute(
                "INSERT OR REPLACE INTO pages(doc_id, page_num, content, word_count) VALUES(?,?,?,?)",
                (doc_id, page_count, page_text, wc)
            )

        db.execute("UPDATE documents SET page_count=? WHERE id=?", (page_count, doc_id))
        db.commit()

        ok += 1
        print(f"  [{i:03d}/{total}] OK   {title[:60]:60s} ({page_count}pp, {total_words}w)")

    db.close()

    # Final stats
    db2 = sqlite3.connect(DB_PATH)
    stats = db2.execute(
        "SELECT COUNT(*), SUM(page_count), (SELECT SUM(word_count) FROM pages) FROM documents"
    ).fetchone()
    db2.close()

    print(f"\n{'='*65}")
    print(f"  DONE: {ok} ingested / {fail} failed / {skip} skipped / {total} total")
    print(f"  KB: {stats[0]} documents, {stats[1]} pages, {stats[2]:,} words")
    print(f"{'='*65}")

if __name__ == "__main__":
    main()
