#!/usr/bin/env python3
"""Classify harvested tools by capability class and identify gate coverage gaps."""
import json
import re
import sys
from typing import Dict, List, Tuple

# Capability classes
EGRESS_INDICATORS = {
    'send_', 'post_', 'upload', 'publish_', 'email', 'webhook', 'notify_', 'alert_',
    'scp', 'rsync', 'ssh_command', 'syndicate', 'broadcast', 'dispatch', 'queue_',
    'trigger_', 'invoke_', 'call_', 'emit_', 'signal_', 'initiate_'
}

SPAWN_INDICATORS = {'agent', 'task', 'spawn', 'create_agent', 'fork', 'exec_bg'}

CONTROL_INDICATORS = {
    'killswitch', 'governance', 'tamper', 'shutdown', 'restart', 'gate_', 'control_',
    'manage_', 'interrupt', 'pause_', 'resume_', 'stop_'
}

WRITE_INDICATORS = {
    'create_', 'delete_', 'update_', 'modify_', 'move_', 'rename_', 'write_', 'save_',
    'store_', 'insert_', 'remove_', 'drop_', 'truncate_', 'clear_', 'reset_'
}

EXEC_INDICATORS = {
    'run_', 'execute_', 'compile', 'build_', 'bash', 'shell', 'eval_', 'apply_',
    'deploy_', 'install_'
}

# High confidence NET-INGRESS (untrusted data)
INGRESS_INDICATORS = {
    'fetch_', 'get_', 'retrieve_', 'download_', 'read_', 'search_', 'query_',
    'list_', 'semantic_', 'find_', 'lookup_', 'resolve_', 'stream_', 'pull_',
    'sync_', 'extract_', 'parse_', 'decode_'
}

def infer_backend(name: str) -> str:
    """Extract backend from tool name."""
    name_lower = name.lower()

    # Check for explicit backend prefixes
    if name.startswith('mcp__'):
        parts = name.split('__')
        if len(parts) >= 2:
            return parts[1].split('_')[0]
    elif name.startswith('mcp_'):
        # mcp_finance_*, mcp_integration_*, mcp_research_*
        parts = name.split('_')
        if len(parts) >= 2:
            return f'mcp-{parts[1]}'
    elif name.startswith('tm_'):
        return 'tm'
    elif name.startswith('tm__'):
        return 'tm'
    elif name.startswith('parallel_ai_'):
        return 'parallel_ai'

    # Heuristic: look for backend keywords in name
    if 'finance' in name_lower or 'trade' in name_lower or 'order' in name_lower:
        return 'distributed-finance'
    elif 'integration' in name_lower or 'email' in name_lower or 'webhook' in name_lower:
        return 'distributed-integration'
    elif 'research' in name_lower or 'entity' in name_lower:
        return 'distributed-research'
    elif 'search' in name_lower or 'semantic' in name_lower:
        return 'distributed-search'
    elif 'timeline' in name_lower or 'event' in name_lower:
        return 'distributed-timeline'
    elif 'memory' in name_lower or 'belief' in name_lower or 'episode' in name_lower:
        return 'distributed-memory'
    elif 'publish' in name_lower or 'broadcast' in name_lower:
        return 'distributed-publish'

    return 'unknown'

def classify_capability(name: str, description: str = "") -> str:
    """Classify tool by primary capability class."""
    name_lower = name.lower()
    desc_lower = description.lower()
    combined = f"{name_lower} {desc_lower}"

    # Check control first (highest risk)
    if any(ind in name_lower for ind in CONTROL_INDICATORS):
        return 'control'

    # Check spawn
    if any(ind in name_lower for ind in SPAWN_INDICATORS):
        return 'spawn'

    # Check egress (data leaving system)
    if any(ind in name_lower for ind in EGRESS_INDICATORS):
        return 'egress'

    # Check exec (includes trading/order execution)
    if any(ind in name_lower for ind in EXEC_INDICATORS):
        return 'exec'
    # Finance execution operations
    if any(word in name_lower for word in ['place_', 'execute_', 'liquidate_', 'exercise_',
                                            'apply_', 'hedge_', 'roll_', 'transfer_']):
        return 'exec'

    # Check write (local state modification)
    if any(ind in name_lower for ind in WRITE_INDICATORS):
        return 'write'
    # Finance/integration write operations
    if any(word in name_lower for word in ['cancel_', 'modify_', 'merge_', 'sync_']):
        return 'write'

    # Check ingress (external untrusted data)
    if any(ind in name_lower for ind in INGRESS_INDICATORS):
        return 'net-ingress'

    # Check read (local or known-safe reads)
    if any(ind in name_lower for ind in {'get_', 'read_', 'list_', 'describe_', 'view_', 'show_', 'info_', 'stream_', 'subscribe_', 'poll_', 'watch_'}):
        return 'read'

    # Default to benign
    return 'benign'

def classify_egress_direction(name: str, capability: str) -> str:
    """Classify data flow direction (out/in/none)."""
    name_lower = name.lower()

    # EGRESS tools send data out
    if capability == 'egress':
        return 'out'

    # NET-INGRESS tools fetch untrusted data in
    if capability == 'net-ingress':
        return 'in'

    # Spawn could exfiltrate via child process
    if capability == 'spawn':
        return 'out'  # potential exfil vector

    # Control operations affect local state
    if capability == 'control':
        return 'none'

    return 'none'

def get_example_input(name: str, capability: str) -> str:
    """Generate example JSON input for the tool."""
    name_lower = name.lower()

    # EGRESS patterns
    if 'send' in name_lower or 'email' in name_lower:
        return '{"to": "user@example.com", "subject": "Test", "body": "Message"}'
    elif 'webhook' in name_lower or 'trigger' in name_lower:
        return '{"url": "https://example.com/webhook", "data": {"action": "event"}}'
    elif 'publish' in name_lower or 'broadcast' in name_lower:
        return '{"topic": "channel", "message": "content"}'
    elif 'upload' in name_lower:
        return '{"file": "data.csv", "destination": "path"}'

    # EXEC patterns (finance/operations)
    elif 'place_' in name_lower or 'execute_' in name_lower:
        return '{"symbol": "AAPL", "quantity": 100, "price": 150.50, "type": "limit"}'
    elif 'cancel_' in name_lower or 'modify_' in name_lower:
        return '{"order_id": "12345", "quantity": 50}'
    elif 'liquidate' in name_lower or 'hedge' in name_lower:
        return '{"position_id": "pos-123", "percentage": 50}'

    # READ patterns
    elif 'fetch' in name_lower or ('get' in name_lower and 'status' not in name_lower):
        return '{"id": "resource-id", "params": {}}'
    elif 'stream' in name_lower or 'subscribe' in name_lower:
        return '{"symbol": "AAPL", "interval": "1min"}'
    elif 'search' in name_lower or 'query' in name_lower:
        return '{"q": "search term", "limit": 10}'

    # WRITE patterns
    elif 'create' in name_lower:
        return '{"name": "New Item", "data": {}}'
    elif 'delete' in name_lower:
        return '{"id": "resource-id"}'
    elif 'update' in name_lower or 'merge' in name_lower:
        return '{"id": "resource-id", "updates": {}}'

    # Default
    else:
        return '{}'

def classify_tools(tools: List[Dict]) -> Tuple[List[Dict], List[Dict]]:
    """Classify all tools and identify gate coverage gaps."""
    classified = []
    gaps = []

    for tool in tools:
        name = tool.get('name', '')
        description = tool.get('description', '')
        backend = infer_backend(name)
        capability_class = classify_capability(name, description)
        egress_dir = classify_egress_direction(name, capability_class)
        example_input = get_example_input(name, capability_class)

        reason = f"Name pattern '{name}' classified as {capability_class}"
        if description:
            reason += f", description: {description[:60]}"

        classified.append({
            'name': name,
            'backend': backend,
            'capability_class': capability_class,
            'egress_direction': egress_dir,
            'example_input': example_input,
            'reason': reason
        })

        # Identify gate coverage gaps
        # Tools that would be misclassified by current gate logic:
        # 1. mcp_/tm_ prefixed tools that are NOT benign (gate treats as untrusted-in only)
        # 2. Tools that do egress but aren't in EGRESS/INTEG_EGRESS sets
        # 3. Spawn tools that could exfil

        if name.startswith(('mcp_', 'mcp__', 'tm__', 'parallel_ai_')):
            # Gate classifies these as CAP_NET | CAP_UNTRUSTED_IN
            # But if they're actually EGRESS or SPAWN, that's wrong
            if capability_class in ('egress', 'spawn', 'write', 'exec', 'control'):
                gaps.append({
                    'name': name,
                    'current_gate_class': 'net-ingress-only',
                    'should_be': capability_class,
                    'reason': f"MCP/TM prefix tool does {capability_class} but gate would only flag as untrusted-in"
                })

    return classified, gaps

if __name__ == '__main__':
    # Read tools from stdin
    try:
        data = json.load(sys.stdin)
        tools = data.get('tools', [])

        if not tools:
            print("No tools found in input", file=sys.stderr)
            sys.exit(1)

        classified, gaps = classify_tools(tools)

        # Sort gaps by severity (egress > spawn > write > exec > control)
        severity_order = {'egress': 0, 'spawn': 1, 'write': 2, 'exec': 3, 'control': 4}
        gaps.sort(key=lambda x: severity_order.get(x['should_be'], 5))

        # Output
        output = {
            'tools': classified,
            'gate_coverage_gaps': gaps[:150]  # Top 150 gaps
        }

        print(json.dumps(output, indent=2))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
