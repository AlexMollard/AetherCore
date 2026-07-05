using System;
using System.Numerics;
using AetherCore.Managed;

namespace AetherScripts;

/// <summary>
/// Physics-toy hotkeys, spawned over this entity's position:
///   1 drop cube   2 drop sphere   3 sphere rain   4 box tower
///   B bounce test  C clear toys
/// Spawned toys are marked transient so they never pollute the saved scene; tag
/// enumeration fills a Span buffer.
/// </summary>
public sealed class ToySpawner : EntityScript
{
    public float ToySize = 0.6f;
    public float DropHeight = 20.0f;

    private MeshHandle _cube;
    private MeshHandle _sphere;
    private TagId _toyTag;

    public override void OnAttach()
    {
        _cube = World.CreateMesh("cube");
        _sphere = World.CreateMesh("sphere");
        _toyTag = Tags.Create("phys_toy");

        InputActions.Register("spawn_cube", Key.Num1);
        InputActions.Register("spawn_sphere", Key.Num2);
        InputActions.Register("spawn_rain", Key.Num3);
        InputActions.Register("spawn_tower", Key.Num4);
        InputActions.Register("bounce_test", Key.B);
        InputActions.Register("clear_toys", Key.C);
    }

    public override void OnUpdate(float dt)
    {
        Vector3 pp = Self.Position;
        if (InputActions.IsPressed("spawn_cube")) { SpawnToy(false, new Vector3(pp.X, DropHeight, pp.Z), ToySize); }
        if (InputActions.IsPressed("spawn_sphere")) { SpawnToy(true, new Vector3(pp.X, DropHeight, pp.Z), ToySize); }
        if (InputActions.IsPressed("spawn_rain")) { SpawnRain(pp); }
        if (InputActions.IsPressed("spawn_tower")) { SpawnTower(pp); }
        if (InputActions.IsPressed("bounce_test")) { SpawnBounce(pp); }
        if (InputActions.IsPressed("clear_toys")) { ClearAllToys(); }
    }

    private void SpawnToy(bool isSphere, Vector3 pos, float size)
    {
        Entity e = World.Create();
        e.Name = isSphere ? "Toy Sphere" : "Toy Cube";
        e.AddTransform();
        e.SetTransform(pos, Vector3.Zero, new Vector3(size, size, size));
        e.MarkTransient();
        if (isSphere)
        {
            e.AddMesh(_sphere);
            e.SetMaterialColor(new Vector3(0.2f, 0.75f, 0.9f));
            Physics.AddSphereBody(e, size * 0.5f, dynamic: true);
        }
        else
        {
            e.AddMesh(_cube);
            Physics.AddBoxBody(e, new Vector3(size * 0.5f, size * 0.5f, size * 0.5f), dynamic: true);
        }
        Tags.Add(e, _toyTag);
    }

    private void SpawnRain(Vector3 basePos)
    {
        for (int i = 0; i < 8; ++i)
        {
            float ang = i * 0.7854f;
            float x = basePos.X + MathF.Cos(ang) * 4.0f;
            float z = basePos.Z + MathF.Sin(ang) * 4.0f;
            SpawnToy(true, new Vector3(x, DropHeight, z), ToySize);
        }
    }

    private void SpawnTower(Vector3 basePos)
    {
        for (int i = 0; i < 8; ++i)
        {
            float y = 1.0f + i * ToySize * 1.05f;
            float jx = (i % 2 - 0.5f) * 0.04f;
            SpawnToy(false, new Vector3(basePos.X + jx, y, basePos.Z), ToySize);
        }
    }

    private void SpawnBounce(Vector3 basePos)
    {
        Entity e = World.Create();
        e.AddTransform();
        e.SetTransform(new Vector3(basePos.X, 1.0f, basePos.Z), Vector3.Zero, new Vector3(ToySize, ToySize, ToySize));
        e.MarkTransient();
        e.AddMesh(_cube);
        Physics.AddBoxBody(e, new Vector3(ToySize * 0.5f, ToySize * 0.5f, ToySize * 0.5f), dynamic: true);
        Tags.Add(e, _toyTag);
        Physics.SetLinearVelocity(e, new Vector3(0.0f, 20.0f, 0.0f));
    }

    private void ClearAllToys()
    {
        Span<Entity> toys = stackalloc Entity[256];
        int count = Tags.GetEntitiesWith(_toyTag, toys);
        for (int i = 0; i < count; ++i)
        {
            if (toys[i].IsValid)
            {
                toys[i].Destroy();
            }
        }
    }
}
