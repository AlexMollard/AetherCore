options gen2;
require daslib/json_boost;
require daslib/strings;
require game_components;
require data_io;

// ── Struct Definitions ────────────────────────────────────────────────────────

struct NpcData
{
    id          : string
    name        : string
    model       : string
    dialogue_id : string
    position    : float3
    rotation    : float3
    scale       : float3
}

struct DialogueResponse
{
    text      : string
    next_node : string
}

struct DialogueNode
{
    speaker_id : string
    text       : string
    responses  : array<DialogueResponse>
}

// ── Loading Helpers ───────────────────────────────────────────────────────────

def private load_float3(jv : JsonValue?; key : string) : float3
{
    let val = jv?[key];
    if (val == null) { return float3(0.0, 0.0, 0.0); }
    var arr <- get_array(val);
    return float3(get_float(arr[0]), get_float(arr[1]), get_float(arr[2]));
}

def private get_string_default(jv : JsonValue?; key : string; def_val : string) : string
{
    let val = jv?[key];
    if (val == null) { return def_val; }
    return get_string(val);
}

// ── NPC Parsing ───────────────────────────────────────────────────────────────

def private parse_npc(jv : JsonValue?) : NpcData
{
    var npcData : NpcData;
    npcData.id = get_string_default(jv, "id", "");
    npcData.name = get_string_default(jv, "name", "");
    npcData.model = get_string_default(jv, "model", "");
    npcData.dialogue_id = get_string_default(jv, "dialogue_id", "");
    npcData.position = load_float3(jv, "position");
    npcData.rotation = load_float3(jv, "rotation");
    npcData.scale = load_float3(jv, "scale");
    return npcData;

}

[export]
def load_npcs(path : string) : array<NpcData>
{
    let text = read_text_file(path);
    if (length(text) == 0) { return <- [[array<NpcData>()]]; }

    var err : string;
    var json_data = read_json(text, err);
    if (json_data == null) { return <- [[array<NpcData>()]]; }

    var arr <- get_array(json_data);
    var result : array<NpcData>;
    for (obj in arr)
    {
        result |> push(parse_npc(obj));
    }
    return <- result;
}

// ── Dialogue Parsing ──────────────────────────────────────────────────────────

def private parse_dialogue_node(jv : JsonValue?) : DialogueNode
{
    var responses : array<DialogueResponse>;
    let responses_jv = jv?["responses"];
    if (responses_jv != null)
    {
        var responses_arr <- get_array(responses_jv);
        for (resp in responses_arr)
        {
            var responseEntry : DialogueResponse;
            responseEntry.text = get_string_default(resp, "text", "");
            responseEntry.next_node = get_string_default(resp, "next_node", "");
            
            responses |> push(responseEntry);
        }
    }

    var node : DialogueNode;
    node.speaker_id = get_string_default(jv, "speaker_id", "");
    node.text = get_string_default(jv, "text", "");
    node.responses = responses;

    return node;
}

[export]
def load_dialogues(path : string) : table<string; DialogueNode>
{
    let text = read_text_file(path);
    if (length(text) == 0) 
    {
        return <- [[table<string; DialogueNode>()]];
    }

    var err : string;
    var json_data = read_json(text, err);
    if (json_data == null)
    {
        return <- [[table<string; DialogueNode>()]];
    }

    var dict <- get_object(json_data);
    var result : table<string; DialogueNode>;
    for (k, v in keys(dict), values(dict))
    {
        if (v != null)
        {
            result[k] <- parse_dialogue_node(v);
        }
    }
    return <- result;
}
