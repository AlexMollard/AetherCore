options gen2;

require data_io;
require daslib/json;

// ── Data structures ───────────────────────────────────────────────────────────

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

// ── Loading helpers ───────────────────────────────────────────────────────────

def private load_float3(jv : JsonValue?; key : string) : float3
{
    if (jv == null) { return float3(0,0,0); }
    let val = (*jv)[key];
    if (val == null) { return float3(0,0,0); }
    let arr = *val;
    return float3(arr[0] |> to_float,
                  arr[1] |> to_float,
                  arr[2] |> to_float);
}

def private parse_npc(jv : JsonValue?) : NpcData
{
    return NpcData(
        id          = jv["id"] |> to_string,
        name        = jv["name"] |> to_string,
        model       = jv["model"] |> to_string,
        dialogue_id = jv["dialogue_id"] |> to_string,
        position    = load_float3(jv, "position"),
        rotation    = load_float3(jv, "rotation"),
        scale       = load_float3(jv, "scale")
    );
}

[export]
def load_npcs(path : string) : array<NpcData>
{
    var result : array<NpcData>;
    let text = read_text_file(path);
    if (length(text) == 0) { return result; }

    let json_data = read_json(text, null);
    if (json_data == null) { return result; }

    let arr = *json_data;
    for (i in 0..length(arr))
    {
        result |> push(parse_npc(arr[i]));
    }
    return result;
}

def private parse_dialogue_node(key : string; jv : JsonValue?) : DialogueNode?
{
    if (jv == null) { return null; }
    let responses_jv = (*jv)["responses"];
    if (responses_jv == null) { return null; }
    let responses_arr = *responses_jv;
    var responses : array<DialogueResponse>;
    for (i in 0..length(responses_arr))
    {
        let rj = responses_arr[i];
        responses |> push(DialogueResponse(
            text      = rj["text"] |> to_string,
            next_node = rj["next_node"] |> to_string
        ));
    }
    return DialogueNode(
        speaker_id = (*jv)["speaker_id"] |> to_string,
        text       = (*jv)["text"] |> to_string,
        responses  = responses
    );
}

[export]
def load_dialogues(path : string) : table<string, DialogueNode>
{
    var result : table<string, DialogueNode>;
    let text = read_text_file(path);
    if (length(text) == 0) { return result; }

    let json_data = read_json(text, null);
    if (json_data == null) { return result; }

    let dict = *json_data;
    let keys = dict |> get_keys();
    for (k in keys)
    {
        let node = parse_dialogue_node(k, dict[k]);
        if (node != null)
        {
            result[k] = *node;
        }
    }
    return result;
}
